#pragma once
// LoRaTrace RX — menu state machine (Phase 6: UI architecture redesign,
// docs/ROADMAP.md; generalized to real nesting 2026-08-25).
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
// (docs/history/CHANGELOG.md, 2026-08-25). That two-level cap held for the
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
// enum used to carry (2026-08-25, docs/BRAND.md's Interface Naming section):
// "Profile" is a GROUP row, not a two-way cycle-on-Enter, so picking a
// profile is two distinct direct selections inside it rather than one
// action that always means "whichever one isn't active."
enum class MenuAction : uint8_t {
    NONE = 0,
    SELECT_MESHTASTIC,
    SELECT_MESHCORE,
    WIFI_TOGGLE,
    DEBUG_TOGGLE,
    IDENTITY_CAPTURE_TOGGLE,
    SD_RETRY,
    SERIAL_CONTROL_TOGGLE,
    TRACE_TOGGLE,
    PROBE_TOGGLE,
    SWEEP_TOGGLE,
    // R key, distinct from S (operator request, 2026-08-29): starts/stops
    // a chain of back-to-back Sweeps instead of one bounded run. Originally
    // a Ctrl+S chord; moved to its own dedicated key 2026-08-30 after real
    // hardware testing showed the TCA8418 can drop Ctrl's release event,
    // leaving repeat mode stuck on (see keyboard.h). ui_task.cpp decides
    // which of this or SWEEP_TOGGLE to fire — not something MenuState
    // itself needs to know about.
    SWEEP_REPEAT_TOGGLE,
    // Cell: a bounded RSSI-only sweep of the North American Cellular
    // downlink band (869-894MHz, cell_plan.h) — a third radio-owned bounded
    // action alongside Probe/Sweep, not a mission profile (radio_task.h's
    // radioRequestCellSweep() comment has the full reasoning). Same shape
    // as Probe/Sweep: global hotkey (C), dedicated carousel results card
    // (UiPage::CELL), no root menu row (ui_task.cpp's ROOT_ITEMS comment).
    CELL_TOGGLE,
    // R key, page-gated to the Cell card (ui_task.cpp decides which of this
    // or CELL_TOGGLE to fire, same as it does for SWEEP_REPEAT_TOGGLE vs.
    // SWEEP_TOGGLE above — not something MenuState itself needs to know
    // about). Probe deliberately has no equivalent (operator decision:
    // "Repeat only on the Sweeps").
    CELL_REPEAT_TOGGLE,
    // Fixed 25/50/75/100 presets replaced by a real slider — UP/DOWN step a
    // live value by 5% instead of jumping between 4 fixed points.
    BRIGHTNESS_UP,
    BRIGHTNESS_DOWN,
    // Sweep's Pass-A peak-detection margin above the rolling noise floor
    // (System > Tuning > Margin, energy_observation.h's
    // ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10/radio_task.h's
    // radioSetEnergySweepMarginDbmX10() — operator request 2026-09-03,
    // docs/STATUS.md's "Sweep silence" investigation). Second SLIDER row,
    // same shape as BRIGHTNESS_UP/DOWN.
    SWEEP_MARGIN_UP,
    SWEEP_MARGIN_DOWN,
    // Cycles repeat-mode Sweep's home-channel capture window
    // (Off/1s/2s/4s, capture_settings.h) — how long each lap pauses on the
    // home channel to actually receive packets. Same "fires and stays in
    // the list" cycling shape as IDLE_TIMEOUT_CYCLE/REGION_CYCLE below.
    // It's a toggle rather than a constant because it trades survey
    // cadence against packet capture, which is an operator call: measured
    // 0/42 packets at Off vs 22/27 at 2s (docs/STATUS.md, v1.0.3).
    CAPTURE_WINDOW_CYCLE,
    // A plain on/off toggle replaced by a cycling value (Off/30s/60s/2min/
    // 5min) — same "fires and stays in the list" shape WIFI_TOGGLE/
    // DEBUG_TOGGLE already have, just cycling instead of flipping a bool.
    IDLE_TIMEOUT_CYCLE,
    // Cycles Sweep's scanned band between US (902-923MHz, 47 CFR § 15.247)
    // and Global (868-923MHz, the module's full tuned range) — same
    // "fires and stays in the list" cycling shape as IDLE_TIMEOUT_CYCLE
    // above. Region is currently Sweep-only; Cell/channel_plans.h
    // regionalization are separate follow-ups (docs/ROADMAP.md).
    REGION_CYCLE,
    // Field Analyzer (Phase 10, docs/research/LoRaTrace-Phases-7-10-
    // Design.md §8.5): fired by the Analyze menu group's own rows
    // (ANALYZE_GROUP_ITEMS, ui_task.cpp), one per row — real UiPage
    // entries, not menu-resident views, since a live-rendered page needs
    // the full 240x135 panel and its own per-page key handling (Scope's
    // Enter-to-acquire) the same way Probe/Sweep/Cell's own cards already
    // do. Navigation-only, same reasoning as OPEN_PROBE/SWEEP/CELL below.
    OPEN_METER,
    OPEN_WATERFALL,
    OPEN_SCOPE,
    OPEN_CAPTURES,
    OPEN_NODES,
    // Fired by SELECT while already on the Scope card (ui_task.cpp),
    // mirroring PROBE_TOGGLE/SWEEP_TOGGLE/CELL_TOGGLE's own dual start/
    // cancel shape — distinct from OPEN_SCOPE above, which only navigates
    // there (arriving on the page already starts a first capture on its
    // own, per SCOPE_ACQUIRE's design). No menu row of its own, same "no
    // duplicate entry point" convention as Probe/Sweep/Cell.
    SCOPE_TOGGLE,
    // Fired by SELECT while on the Waterfall card (operator request,
    // 2026-09-04): Waterfall is Sweep's own history view, so starting/
    // stopping repeat Sweep straight from here — without leaving to the
    // Tools/Sweep card first — is the whole point. Same
    // radioRequestEnergySweepRepeat() call as SWEEP_REPEAT_TOGGLE below,
    // but deliberately its own action rather than reused directly: that
    // one's handler also calls showSweepResults() to jump the operator
    // onto the Sweep card, which is correct for its own (menu/global-R-key)
    // context but wrong here — the operator pressed Enter on Waterfall
    // specifically to keep watching it fill in, not to be navigated away.
    WATERFALL_SWEEP_REPEAT_TOGGLE,
    // Same shape as OPEN_METER etc. above, just for Probe/Sweep/Cell —
    // fired by the Tools menu group's own rows (TOOLS_GROUP_ITEMS,
    // ui_task.cpp). Deliberately navigation-only, not PROBE_TOGGLE/
    // SWEEP_TOGGLE/CELL_TOGGLE: unlike Scope, none of these three auto-
    // start on arrival (only Scope's SCOPE_ACQUIRE design does that), so
    // opening one from the menu should land on an idle/last-result card
    // exactly like using a JUMP_n or paging there directly would, not
    // silently kick off a scan. Reuses showProbeResults()/
    // showSweepResults()/showCellResults() (already exist — the P/S/C
    // hotkeys already jump to these same three cards), not new functions.
    OPEN_PROBE,
    OPEN_SWEEP,
    OPEN_CELL,
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
    // (ui_task.cpp applies the step and clamps); BACK or SELECT both leave
    // the slider and return to the list it was opened from (same depth,
    // cursor unchanged) — same "one step back" contract handleList()'s BACK
    // has. SELECT added 2026-08-29 (operator request: Enter should confirm
    // a value, not just ESC out of it) — ui_task.cpp's own leavingSlider
    // check (the SD-write debounce point) was updated alongside this to
    // recognize both keys, not just BACK.
    MenuAction handleSlider(KeyAction key) {
        const MenuItem &item = currentItem();
        switch (key) {
            case KeyAction::NEXT:
                return item.sliderIncrease;
            case KeyAction::PREV:
                return item.sliderDecrease;
            case KeyAction::BACK:
            case KeyAction::SELECT:
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
