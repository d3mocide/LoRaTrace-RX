#pragma once
// LoRaTrace RX — menu state machine (Phase 6: UI architecture redesign,
// ROADMAP.md; generalized to real nesting 2026-08-25).
//
// Pure logic, no Arduino/display dependency — same "decode/state machine
// separate from drawing and hardware" split as keyboard.h/gps_parse.h, so
// this is host-testable (pio test -e native, test/test_ui_menu/) instead of
// only bench-testable.
//
// Originally shipped as an explicit two-level design ("root, then one group
// inside it, never more") — replacing Phase 5's flat, fixed-size menu,
// which shipped scoped to exactly two items and had already grown a third
// (verbose debug) the same bench day with no framework change to absorb it
// (PROGRESS.md's 2026-08-25 Decisions log). That two-level cap held for the
// rest of Phase 6 until the operator asked to move Brightness/idle-dim
// under a "System > Display > ..." grouping — a real third level, not
// something a special case for one screen was worth building around given
// it's now the *second* time a nesting need has come up (System's own
// WiFi/Debug/Trace grouping was the first). Generalized to arbitrary-depth
// (bounded by MAX_DEPTH) recursive navigation instead: every row, at any
// depth, is the same MenuItem shape — ACTION rows fire immediately, GROUP
// rows open their own nested item list, SLIDER rows step a live value with
// NEXT/PREV once entered. A GROUP can contain more GROUPs, more SLIDERs, or
// a mix, all through the same recursive shape.
//
// Input model unchanged from Phase 5 (keyboard.h): PREV/NEXT move a
// selection (or step a slider), SELECT activates the highlighted row, BACK
// peels back exactly one level. JUMP_1..5 and NONE are always no-ops here —
// carousel page jumps stay ui_task.cpp's concern, same as Phase 5.

#include <stdint.h>

#include "keyboard.h"

// What a menu row does when activated.
//
// SELECT_MESHTASTIC/SELECT_MESHCORE replace the single PROFILE_SWITCH this
// enum used to carry (2026-08-25, BRAND.md's Interface Naming section):
// "Profile" is a GROUP row, not a two-way cycle-on-Enter, so picking a
// profile is two distinct direct selections inside it rather than one
// action that always means "whichever one isn't active."
enum class MenuAction : uint8_t {
    NONE = 0,
    SELECT_MESHTASTIC,
    SELECT_MESHCORE,
    WIFI_TOGGLE,
    DEBUG_TOGGLE,
    SD_RETRY,
    LOW_PROFILE_TOGGLE,
    TRACE_TOGGLE,
    PROBE_TOGGLE,
    SWEEP_TOGGLE,
    // Fixed 25/50/75/100 presets replaced by a real slider — UP/DOWN step a
    // live value by 5% instead of jumping between 4 fixed points.
    BRIGHTNESS_UP,
    BRIGHTNESS_DOWN,
    // A plain on/off toggle replaced by a cycling value (Off/30s/60s/2min/
    // 5min) — same "fires and stays in the list" shape WIFI_TOGGLE/
    // DEBUG_TOGGLE already have, just cycling instead of flipping a bool.
    IDLE_TIMEOUT_CYCLE,
};

enum class ItemKind : uint8_t { ACTION, GROUP, SLIDER };

// One menu row, at any depth — root-level or nested inside any GROUP.
// ACTION rows carry `action` and ignore the rest; GROUP rows carry a nested
// list (`items`/`itemCount`) and ignore `action`/`sliderIncrease`/
// `sliderDecrease`; SLIDER rows carry `sliderIncrease`/`sliderDecrease`
// (fired on NEXT/PREV once entered) and ignore the others. A slider's
// actual value, bounds, and step size are deliberately NOT here — that's
// runtime state the caller (ui_task.cpp) owns, same as a GROUP's current
// selection isn't part of this table either.
struct MenuItem {
    const char *label;
    ItemKind kind;
    MenuAction action;
    MenuAction sliderIncrease;
    MenuAction sliderDecrease;
    const MenuItem *items;
    uint8_t itemCount;
};

class MenuState {
public:
    // Max navigable depth (root list = depth 1). 4 is generous headroom
    // over the 3 actually used today (root -> System -> Display) — cheap
    // to raise later (it's just a small fixed-size array), not a hardcoded
    // ceiling worth routing around.
    static constexpr uint8_t MAX_DEPTH = 4;

    // `roots`/`rootCount` must outlive this object — same convention as
    // channel_plans.h's constexpr tables, expected to be a static/constexpr
    // array owned by the caller (ui_task.cpp), not copied in. Every nested
    // GROUP's `items` array has the same lifetime expectation.
    MenuState(const MenuItem *roots, uint8_t rootCount) : roots_(roots), rootCount_(rootCount) {}

    bool isOpen() const { return depth_ > 0; }
    // 1 = the root list itself is what's on screen; 2 = one level in; etc.
    uint8_t depth() const { return depth_; }
    // True once SELECT has entered a SLIDER row — currentItem() is what's
    // being adjusted; currentList()/currentCount()/currentIndex() still
    // describe the list *behind* it (unchanged while inSlider(), so BACK
    // can return to exactly where it was).
    bool inSlider() const { return inSlider_; }

    const MenuItem *currentList() const {
        return depth_ <= 1 ? roots_ : nodeAt(depth_ - 2).items;
    }
    uint8_t currentCount() const {
        return depth_ <= 1 ? rootCount_ : nodeAt(depth_ - 2).itemCount;
    }
    uint8_t currentIndex() const { return index_[depth_ - 1]; }
    const MenuItem &currentItem() const { return currentList()[currentIndex()]; }

    // Ancestor labels for a breadcrumb, oldest first — e.g. depth()==3
    // (Display's list open, nested in System) yields count 2:
    // {"System", "Display"}. Empty at depth() <= 1 (nothing to breadcrumb
    // yet — the root list itself needs none).
    uint8_t breadcrumbCount() const { return depth_ > 1 ? (uint8_t)(depth_ - 1) : 0; }
    const char *breadcrumbLabel(uint8_t i) const { return nodeAt(i).label; }

    void open() {
        depth_ = 1;
        for (uint8_t i = 0; i < MAX_DEPTH; i++) index_[i] = 0;
        inSlider_ = false;
    }

    void close() {
        depth_ = 0;
        inSlider_ = false;
    }

    // Feeds one key action into whichever level/mode is currently active.
    // Returns the MenuAction that just fired (NONE if the key only moved a
    // selection, opened/closed a level, or was ignored). ui_task.cpp is
    // expected to call this only while isOpen() — carousel-mode keys
    // (PREV/NEXT paging, JUMP_n, and the BACK-opens-the-menu transition)
    // stay its own concern, same split Phase 5 already had between
    // UiMode::CAROUSEL and UiMode::MENU.
    MenuAction handle(KeyAction key) {
        if (depth_ == 0) return MenuAction::NONE;
        if (inSlider_) return handleSlider(key);
        return handleList(key);
    }

private:
    // level 0 = the item chosen from the root list (roots_[index_[0]]);
    // level 1 = the item chosen from THAT item's own list, etc. Used to
    // resolve currentList()/currentCount() at depth_ > 1, and for
    // breadcrumbLabel().
    const MenuItem &nodeAt(uint8_t level) const {
        const MenuItem *list = roots_;
        const MenuItem *node = &list[index_[0]];
        for (uint8_t d = 1; d <= level; d++) {
            list = node->items;
            node = &list[index_[d]];
        }
        return *node;
    }

    MenuAction handleList(KeyAction key) {
        const MenuItem *list = currentList();
        const uint8_t count = currentCount();
        // An empty GROUP is a caller bug — fail safe back one level rather
        // than indexing nothing.
        if (count == 0) {
            if (depth_ > 1) depth_--; else close();
            return MenuAction::NONE;
        }
        uint8_t &idx = index_[depth_ - 1];
        switch (key) {
            case KeyAction::PREV:
                idx = (uint8_t)((idx + count - 1) % count);
                break;
            case KeyAction::NEXT:
                idx = (uint8_t)((idx + 1) % count);
                break;
            case KeyAction::BACK:
                if (depth_ > 1) depth_--; else close();
                break;
            case KeyAction::SELECT: {
                const MenuItem &item = list[idx];
                if (item.kind == ItemKind::ACTION) {
                    return item.action;
                }
                if (item.kind == ItemKind::SLIDER) {
                    inSlider_ = true;
                    break;
                }
                // GROUP — open its nested list, one level deeper.
                if (item.itemCount > 0 && depth_ < MAX_DEPTH) {
                    depth_++;
                    index_[depth_ - 1] = 0;
                }
                break;
            }
            default:
                break; // NONE, JUMP_1..5 — no-ops, same as Phase 5's menu
        }
        return MenuAction::NONE;
    }

    // No item list to navigate — just a live value the caller owns.
    // NEXT/PREV fire the item's increase/decrease action every press
    // (ui_task.cpp applies the step and clamps); BACK leaves the slider and
    // returns to the list it was opened from (same depth, cursor
    // unchanged) — same "one step back" contract handleList()'s BACK has.
    MenuAction handleSlider(KeyAction key) {
        const MenuItem &item = currentItem();
        switch (key) {
            case KeyAction::NEXT:
                return item.sliderIncrease;
            case KeyAction::PREV:
                return item.sliderDecrease;
            case KeyAction::BACK:
                inSlider_ = false;
                break;
            default:
                break;
        }
        return MenuAction::NONE;
    }

    const MenuItem *roots_;
    uint8_t rootCount_;
    uint8_t depth_ = 0; // 0 = closed
    uint8_t index_[MAX_DEPTH] = {0};
    bool inSlider_ = false;
};
