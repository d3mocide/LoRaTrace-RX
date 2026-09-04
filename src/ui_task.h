#pragma once
// LoRaTrace RX — UI task (Core 0).
//
// PHASE-ORDER NOTE: read-only status pages arrived in Phase 2, pulled
// forward from wherever CLAUDE.md originally proposed real UI, because
// field-testing for hours means reading the device *without* a tethered
// laptop — which the boot splash alone couldn't support. Phase 3/4 each
// added one binary keyboard gesture on top (WiFi toggle, profile switch) as
// narrow exceptions specifically because this project had no sourced
// Cardputer-ADV row/col-to-character keymap yet, so nothing beyond
// undifferentiated press/release timing was trustworthy.
//
// Phase 5 replaced the old timed hold-gestures with a real menu, using
// keyboard.h's sourced (partly bench-verified — see its own comments and
// docs/history/PROGRESS.md's Phase 5 checklist) decode for twelve specific keys: ',' or
// ';' / '.' or '/' move (the Fn-arrow diamond's left/up and down/right,
// both pairs alias the same action), Enter selects, the backtick/ESC key
// goes back, '1'-'5' jump straight to a numbered carousel page.
//
// Phase 6 (docs/ROADMAP.md, UI architecture redesign) replaces Phase 5's flat,
// fixed-size menu with a grouped one (ui_menu.h's MenuState) — Phase 5's
// menu shipped scoped to exactly two toggles and had already grown a third
// (verbose debug) the same bench day, with no framework change to absorb
// it (docs/history/CHANGELOG.md, 2026-08-25). Same four input keys
// throughout, now three levels instead of two:
//   - **Carousel** (default): ','/';'/'.'/'/' cycle the read-only status
//     pages below; digits '1'-'5' jump straight to one of them; the
//     backtick/ESC key opens the menu at its root; Enter toggles Trace on
//     RADIO and starts/cancels Probe on PROBE (no-op on other cards), while
//     P starts/cancels Probe from any UI state.
//   - **Menu root**: the same move keys move a highlighted root row; Enter
//     opens a GROUP row's sub-list — Profile, Analyze, Tools, System are
//     all groups now (Tools/Analyze joined 2026-09-05, folding in what used
//     to be their own carousel hub pages; Trace moved into Tools as a
//     child row — see ROOT_ITEMS in ui_task.cpp and docs/BRAND.md's
//     Interface Naming section); the backtick/ESC key closes the menu back
//     to the carousel. Digit keys are ignored here, same as Phase 5.
//   - **Menu group** (inside a GROUP row): the same move keys move a
//     highlighted item within the group; Enter fires it (a direct profile
//     switch to Meshtastic/MeshCore, the WiFi/Debug toggles, or navigates
//     onto a real page for Tools/Analyze's own rows); the backtick/ESC key
//     returns to the menu root, not all the way to the carousel.
// A toast overlay (ui_task.cpp's showToast()) confirms whatever action just
// fired, independent of which page/menu level is on screen afterward.
// Deliberately not a general keymap or text-entry UI — see keyboard.h for
// why twelve keys are enough for this scope (docs/DESIGN.md/docs/history/PROGRESS.md Phase 5).
//
// Owns the ST7789 exclusively once started — main.cpp must stop drawing.
// The display is on its own SPI host (HSPI) with pins disjoint from the
// radio/SD bus, so it needs no spi_bus arbitration.

#include <Arduino_GFX_Library.h>

#include "display_settings.h"

// Pages the operator can cycle through in carousel mode. Order is
// deliberate: RADIO first because it's the reason the device exists,
// then the retained second-card PROBE results page for the bounded discovery
// action, followed by CHANNEL (RADIO's RF detail), GPS, and SYSTEM. WIFI is gone as
// its own page (Phase 6 UI redesign, 2026-08-25) — folded into SYSTEM as a
// fourth stat block, since AP-on/off plus client count didn't need a whole
// carousel slot of its own once SYSTEM had room for a 2x2 grid.
//
// CELL inserted after SWEEP (Phase 11, 2026-09-01, operator request) — same
// "own bounded action gets its own results card" precedent as PROBE/SWEEP.
// Values are unnumbered on purpose so an insertion here is never itself an
// edit anywhere else; every downstream digit/JUMP mapping lives in
// ui_task.cpp/keyboard.h instead.
//
// The *operator-facing* main carousel is five stops: Radio, Activity,
// Channel, GPS, System (JUMP_1..5, JUMP_6 unmapped) — PROBE/SWEEP/CELL and
// METER/WATERFALL/SCOPE/CAPTURES/NODES are real UiPage values (each still
// needs its own draw function and footer identity) but are reached only
// through the menu (Tools/Analyze GROUPs, ROOT_ITEMS' own comment in
// ui_task.cpp, 2026-09-05) — they briefly had their own carousel hub pages
// (Phase 10/2026-09-04, CHANGELOG.md), which is why they're still real
// UiPage values instead of menu-resident rows: a live-rendered page needs
// the full 240x135 panel and its own per-page key handling (Scope's
// Enter-to-acquire), the same way Radio/Channel/GPS/System do. Once on
// one, UP/DOWN cycle the other pages in the same group
// (nextToolsSubPage()/nextAnalyzeSubPage() etc., ui_task.cpp) and
// PREV/NEXT/BACK all reopen the menu at its root, since none of them has a
// carousel position to return to.
//
// ACTIVITY (added 2026-09-05, main carousel slot 2, operator request) is a
// read-only mirror of whichever bounded action is currently running
// (Probe/Sweep/Cell/Scope) — an ordinary carousel page like Radio/Channel/
// GPS/System, not a gated sub-page: it never starts or cancels anything
// itself, just reports state, so it carries none of the "duplicate entry
// point" risk Probe/Sweep/Cell's own dedicated cards would. Complements
// Radio's own STANDBY/repeat banner (drawRadioPage()) with more room to
// show it, rather than replacing that banner.
enum class UiPage : uint8_t {
    RADIO = 0,
    ACTIVITY,
    PROBE,
    SWEEP,
    CELL,
    CHANNEL,
    GPS,
    SYSTEM,
    METER,
    WATERFALL,
    SCOPE,
    CAPTURES,
    NODES,
    COUNT,
};

// Starts the task on Core 0. `gfx` must already be initialised (main.cpp
// brings it up for the boot splash). `settings` is main.cpp's boot-time SD
// load (display_settings.h) — seeds the Brightness slider/idle-timeout
// state instead of a hardcoded default, same pattern radioTaskStart()
// already receives its boot-time ProfileOverrides through. Returns false
// if the task could not be created; the caller should carry on regardless
// — a headless wardriver still logs, which is the actual job.
bool uiTaskStart(Arduino_GFX *gfx, const DisplaySettings &settings);

// True if the TCA8418 keyboard controller came up. When false the UI falls
// back to auto-advancing pages on a timer, so the device stays useful
// rather than stuck on one page.
bool uiKeyboardReady();

// Raw keyboard event dump, off by default and toggled over Serial Control
// (KEY_DUMP). Emits one `[keydump]` line per TCA8418 FIFO event with its raw
// byte, key number, press/release edge, and decoded (row, col) — the
// empirical check keyboard.h's paper-derived constants never had. This is
// what caught the Ctrl+S modifier chord dropping its own release event on
// real hardware (2026-08-30), leading to that chord's removal.
void uiKeyDumpSetEnabled(bool enabled);
bool uiKeyDumpIsEnabled();
