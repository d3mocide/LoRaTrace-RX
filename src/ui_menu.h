#pragma once
// LoRaTrace RX — grouped menu state machine (Phase 6: UI architecture
// redesign, ROADMAP.md).
//
// Pure logic, no Arduino/display dependency — same "decode/state machine
// separate from drawing and hardware" split as keyboard.h/gps_parse.h, so
// this is host-testable (pio test -e native, test/test_ui_menu/) instead of
// only bench-testable.
//
// Replaces Phase 5's flat, fixed-size menu (ui_task.cpp's old
// `MENU_ITEM_COUNT`/`menuSelection`), which shipped scoped to exactly two
// items and had already grown a third (verbose debug) the same bench day
// with no framework change — see PROGRESS.md's 2026-08-25 Decisions log.
// Root items are either a DIRECT action (fires immediately, mirrors
// M5PORKCHOP's `RootType::DIRECT`, github.com/0ct0sec/M5PORKCHOP
// src/ui/menu.h — reviewed for this structural idea only, not its content)
// or a GROUP that opens a short sub-list — two levels, never more, so
// navigation never needs a stack deeper than "root" and "inside one group."
//
// Input model unchanged from Phase 5 (keyboard.h): PREV/NEXT move a
// selection, SELECT activates it, BACK peels back exactly one level (group
// -> root -> closed). JUMP_1..5 and NONE are always no-ops here — carousel
// page jumps stay ui_task.cpp's concern, same as Phase 5.

#include <stdint.h>

#include "keyboard.h"

// What a menu row (root DIRECT item or group item) does when activated.
// NONE is "this row exists but selecting it does nothing on its own" — used
// for a GROUP root row, whose SELECT opens the sub-list rather than firing
// an action directly.
//
// SELECT_MESHTASTIC/SELECT_MESHCORE replace the single PROFILE_SWITCH this
// enum used to carry (2026-08-25, BRAND.md's Interface Naming section):
// "Profile" is a GROUP root row now, not a two-way cycle-on-Enter, so
// picking a profile is two distinct direct selections inside it rather
// than one action that always means "whichever one isn't active."
enum class MenuAction : uint8_t {
    NONE = 0,
    SELECT_MESHTASTIC,
    SELECT_MESHCORE,
    WIFI_TOGGLE,
    DEBUG_TOGGLE,
};

enum class RootKind : uint8_t { DIRECT, GROUP };

// One item inside a group's sub-list.
struct MenuEntry {
    const char *label;
    MenuAction action;
};

// One root-level row. DIRECT rows carry `directAction` and ignore
// `groupItems`/`groupCount`; GROUP rows carry a sub-list and ignore
// `directAction`.
struct RootEntry {
    const char *label;
    RootKind kind;
    MenuAction directAction;
    const MenuEntry *groupItems;
    uint8_t groupCount;
};

enum class MenuLevel : uint8_t { CLOSED, ROOT, GROUP };

class MenuState {
public:
    // `roots`/`rootCount` must outlive this object — same convention as
    // channel_plans.h's constexpr tables, expected to be a static/constexpr
    // array owned by the caller (ui_task.cpp), not copied in.
    MenuState(const RootEntry *roots, uint8_t rootCount) : roots_(roots), rootCount_(rootCount) {}

    MenuLevel level() const { return level_; }
    bool isOpen() const { return level_ != MenuLevel::CLOSED; }
    uint8_t rootIndex() const { return rootIdx_; }
    uint8_t groupIndex() const { return groupIdx_; }

    // Valid whenever rootCount_ > 0, i.e. whenever the caller should be
    // calling this at all — a zero-length root table is a caller bug, not a
    // state this class tries to paper over.
    const RootEntry &currentRoot() const { return roots_[rootIdx_]; }

    void open() {
        level_ = MenuLevel::ROOT;
        rootIdx_ = 0;
        groupIdx_ = 0;
    }

    void close() { level_ = MenuLevel::CLOSED; }

    // Feeds one key action into whichever level is currently active.
    // Returns the MenuAction that just fired (NONE if the key only moved a
    // selection, opened/closed a level, or was ignored). ui_task.cpp is
    // expected to call this only while isOpen() — carousel-mode keys
    // (PREV/NEXT paging, JUMP_n, and the BACK-opens-the-menu transition)
    // stay its own concern, same split Phase 5 already had between
    // UiMode::CAROUSEL and UiMode::MENU.
    MenuAction handle(KeyAction key) {
        if (level_ == MenuLevel::ROOT) return handleRoot(key);
        if (level_ == MenuLevel::GROUP) return handleGroup(key);
        return MenuAction::NONE;
    }

private:
    MenuAction handleRoot(KeyAction key) {
        if (rootCount_ == 0) return MenuAction::NONE;
        switch (key) {
            case KeyAction::PREV:
                rootIdx_ = (uint8_t)((rootIdx_ + rootCount_ - 1) % rootCount_);
                break;
            case KeyAction::NEXT:
                rootIdx_ = (uint8_t)((rootIdx_ + 1) % rootCount_);
                break;
            case KeyAction::BACK:
                close();
                break;
            case KeyAction::SELECT: {
                const RootEntry &r = roots_[rootIdx_];
                if (r.kind == RootKind::DIRECT) {
                    return r.directAction;
                }
                if (r.groupCount > 0) {
                    level_ = MenuLevel::GROUP;
                    groupIdx_ = 0;
                }
                break;
            }
            default:
                break; // NONE, JUMP_1..5 — no-ops, same as Phase 5's menu
        }
        return MenuAction::NONE;
    }

    MenuAction handleGroup(KeyAction key) {
        const RootEntry &r = roots_[rootIdx_];
        // A GROUP row with an empty sub-list is a caller bug (see class
        // comment) — fail safe back to root rather than indexing nothing.
        if (r.groupCount == 0) {
            level_ = MenuLevel::ROOT;
            return MenuAction::NONE;
        }
        switch (key) {
            case KeyAction::PREV:
                groupIdx_ = (uint8_t)((groupIdx_ + r.groupCount - 1) % r.groupCount);
                break;
            case KeyAction::NEXT:
                groupIdx_ = (uint8_t)((groupIdx_ + 1) % r.groupCount);
                break;
            case KeyAction::BACK:
                level_ = MenuLevel::ROOT; // one level back, not fully closed
                break;
            case KeyAction::SELECT:
                return r.groupItems[groupIdx_].action;
            default:
                break;
        }
        return MenuAction::NONE;
    }

    const RootEntry *roots_;
    uint8_t rootCount_;
    MenuLevel level_ = MenuLevel::CLOSED;
    uint8_t rootIdx_ = 0;
    uint8_t groupIdx_ = 0;
};
