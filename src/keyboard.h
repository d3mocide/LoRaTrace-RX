#pragma once
// LoRaTrace RX — keyboard input decode (Phase 5).
//
// Turns a raw TCA8418 event byte (Adafruit_TCA8418::getEvent()) into one of
// recognized UI actions for the on-device menu (ui_task.cpp). Pure logic,
// no Arduino dependency, so it's host-testable (pio test -e native) same as
// detection.h/gps_parse.h.
//
// Deliberately NOT a general keymap. CLAUDE.md's house rule is not to guess
// hardware tables, and until now this project had no sourced Cardputer-ADV
// row/col-to-character map — every past on-device gesture (ui_task.cpp's now
// -removed KeyGesture/pollKeyGesture()) was built on undifferentiated
// press/release timing specifically to avoid needing one. Phase 5's UX
// decision (plain ','/'.' to move, Enter to select, Backspace to go back —
// no Fn chord, no numeric entry) only ever needed FOUR specific keys
// identified, not the full 56-key table, which is what made a real, sourced
// decode tractable as a small, low-risk slice instead of a project of its
// own. Extended twice since (still Phase 5, pre-hardware-verification),
// same sourcing chain, same "no Fn chord" rule, still nowhere near the full
// 56-key table:
//   - Digits '1'-'5' as direct carousel-page jumps, requested once a full
//     keyboard was on hand for the same bench session that verifies the
//     rest. **Bench-confirmed 2026-08-24**: all five work as expected.
//   - First-round bench testing of the above also surfaced two real UX
//     asks, acted on the same session: Backspace felt wrong for "leave the
//     menu" (swapped for the top-left backtick key, physically labelled
//     ESC on the Cardputer-ADV's keycap even though the TCA8418 reports it
//     as backtick — RetroBreeze's cardputer-keyboard-reference documents
//     this exact quirk: "There is no dedicated ESC key in the firmware...
//     ESC is accessed as Fn + backtick" — bound here as a plain press, no
//     Fn, matching this project's existing no-chord rule rather than the
//     upstream Fn+backtick convention); and the operator, working from the
//     same keycaps, tried the printed Fn-arrow diamond (';'/','/'.'/'/' =
//     up/left/down/right, also documented in RetroBreeze's reference)
//     expecting it to double as page/menu
//     navigation — ',' and '.' already did (they're PREV/NEXT), so ';' and
//     '/' are now wired in as plain-press aliases for the same two actions,
//     completing the set. No new KeyAction values needed for the aliases —
//     see keyboardDecodeEvent() below.
//
// --- Sourcing, so a future reader doesn't have to re-derive this ---
//
// 1. Raw event byte encoding: documented in Adafruit_TCA8418::getEvent()'s
//    own doc comment (the library this project already depends on for the
//    keyboard wake sequence, board_pins.h) — key press events are 0x01..0x50
//    (i.e. the TCA8418's internal key number K, 1-80, directly); release
//    events are the same K plus 0x80 (0x81..0xD0).
//
// 2. Raw K -> physical (row, col) in the Cardputer-ADV's 4-row x 14-col key
//    layout: verbatim from bmorcelli/Launcher's own shipped, running
//    Cardputer-ADV interface code (boards/m5stack-cardputer/interface.cpp,
//    mapRawKeyToPhysical()) — the same repo this project already sources the
//    TCA8418 wake sequence, GPIO5/NSS timing, and TFT offsets from:
//        u = K % 10; t = K / 10;        // valid only for u in 1..8, t<=6
//        u0 = u - 1;
//        row = u0 & 0x03;               // 0..3
//        col = (t << 1) | (u0 >> 2);    // 0..13
//
// 3. Physical (row, col) -> which key it physically is: from RetroBreeze's
//    cardputer-keyboard-reference (github.com/RetroBreeze/
//    cardputer-keyboard-reference), which documents the Cardputer-ADV's full
//    _key_value_map[4][14] specifically (not the base Cardputer's GPIO-matrix
//    variant). Independently cross-checked against Launcher's own
//    InputHandler, which recognizes both Enter and Backspace by "col == 13"
//    — matching the table below exactly, from a second, independent source:
//      Backspace: (row 0, col 13)   Enter:      (row 2, col 13)
//      Comma ',': (row 3, col 10)   Period '.': (row 3, col 11)
//    Row 0 of the same _key_value_map table gives the digit row (also the
//    backtick/ESC key) and row 2/row 3 give the rest of the Fn-arrow
//    diamond:
//      Row 0: {'`'}{'1'}{'2'}{'3'}{'4'}{'5'}{'6'}{'7'}{'8'}{'9'}{'0'}{'-'}
//             {'='}{BKSP} at columns 0-13 — backtick at col 0, '1'-'5' at
//             col 1-5.
//      Semicolon ';': (row 2, col 11) — same row as Enter, five columns
//             left of it. Slash '/': (row 3, col 12) — same row as
//             Comma/Period, one column right of Period.
//    RetroBreeze's reference also documents the Fn layer directly (section
//    7/9): "ESC is accessed as Fn + backtick" (the top-left key has no
//    dedicated ESC code — the firmware reports plain backtick), and
//    "';' ',' '.' '/' double as arrow keys when Fn is held" mapping exactly
//    up/left/down/right — independent confirmation that these are the
//    intended roles of these five keys' printed keycaps, not a guess about
//    what the silkscreen shows.
//
// Inverting step 2's formula for each (row, col) pair (t = col>>1,
// topbit = col&1, u = ((topbit<<2)|row)+1, K = t*10+u; checked by plugging
// each result back into the forward formula above) gives the raw press-byte
// constants below. **Bench-confirmed on real hardware, 2026-08-24:** Comma,
// Period, and all five digit keys (PROGRESS.md Decisions log). Enter was
// exercised indirectly (reaching the menu at all requires it) but not
// separately confirmed. Backtick/ESC and Semicolon/Slash are brand new this
// same session, sourced but **not yet pressed on real hardware** — same bar
// as everything else in this file before its own bench pass: see
// PROGRESS.md's Phase 5 checklist.

#include <stdint.h>

// Enter: physical (row 2, col 13) -> t=6, u=7 -> K=67.
constexpr uint8_t KEY_RAW_ENTER_PRESS = 67;
// Comma ',': physical (row 3, col 10) -> t=5, u=4 -> K=54.
constexpr uint8_t KEY_RAW_COMMA_PRESS = 54;
// Period '.': physical (row 3, col 11) -> t=5, u=8 -> K=58.
constexpr uint8_t KEY_RAW_PERIOD_PRESS = 58;

// Backtick/ESC: physical (row 0, col 0) -> t=0, u=1 -> K=1. Bound as BACK
// (superseding Backspace, K=65 — bench testing 2026-08-24 found Backspace
// felt wrong for "leave the menu"; the top-left key is silkscreened ESC on
// the Cardputer-ADV even though the TCA8418/RetroBreeze's own reference
// call its base character backtick). Plain press, no Fn — this project
// doesn't track Fn as a modifier at all (see the "no Fn chord" note above),
// so this is not the upstream Fn+backtick ESC combo, just the same key
// pressed alone.
constexpr uint8_t KEY_RAW_ESC_PRESS = 1;

// Semicolon ';' and Slash '/': the rest of the printed Fn-arrow diamond
// (up/right; ',' /'.' already cover left/down as Comma/Period above).
// Aliases for the same KeyAction::PREV/NEXT the carousel and menu already
// use, not new actions — see keyboardDecodeEvent() below.
// ';': physical (row 2, col 11) -> t=5, u=7 -> K=57.
constexpr uint8_t KEY_RAW_SEMICOLON_PRESS = 57;
// '/': physical (row 3, col 12) -> t=6, u=4 -> K=64.
constexpr uint8_t KEY_RAW_SLASH_PRESS = 64;

// Digit row, page-jump keys (carousel mode only — see keyboardDecodeEvent()).
// '1': physical (row 0, col 1) -> t=0, u=5 -> K=5.
constexpr uint8_t KEY_RAW_1_PRESS = 5;
// '2': physical (row 0, col 2) -> t=1, u=1 -> K=11.
constexpr uint8_t KEY_RAW_2_PRESS = 11;
// '3': physical (row 0, col 3) -> t=1, u=5 -> K=15.
constexpr uint8_t KEY_RAW_3_PRESS = 15;
// '4': physical (row 0, col 4) -> t=2, u=1 -> K=21.
constexpr uint8_t KEY_RAW_4_PRESS = 21;
// '5': physical (row 0, col 5) -> t=2, u=5 -> K=25.
constexpr uint8_t KEY_RAW_5_PRESS = 25;

// P: physical (row 1, col 10) -> t=5, u=2 -> K=52. It is the global Probe
// shortcut: ui_task.cpp starts/cancels the bounded action from any UI state.
constexpr uint8_t KEY_RAW_P_PRESS = 52;

// S: physical (row 2, col 3) per RetroBreeze's cardputer-keyboard-reference
// `_key_value_map[4][14]` (row 2: {FN}{SHF}{'a'}{'s'}...) — same source this
// whole file already cites. Cross-checked before trusting it for a new key:
// that same table's row 1/col 10 is 'p', matching this file's own already
// bench-confirmed KEY_RAW_P_PRESS=52 exactly. Inverting the documented
// formula (t=col>>1=1, topbit=col&1=1, u=((topbit<<2)|row)+1=7, K=t*10+u=17)
// gives 17; checked by re-running the forward formula back to (row 2, col 3).
// Global Sweep shortcut, same shape as P/Probe above. Sourced but **not yet
// bench-confirmed on real hardware** — same bar as every other newly-added
// key in this file before its own bench pass.
constexpr uint8_t KEY_RAW_S_PRESS = 17;

enum class KeyAction {
    NONE,
    // ',' or ';' (Fn-arrow "left"/"up") — previous status page (carousel) /
    // move selection up (menu). Two physical keys, one action: see
    // KEY_RAW_SEMICOLON_PRESS above for why.
    PREV,
    // '.' or '/' (Fn-arrow "down"/"right") — next status page (carousel) /
    // move selection down (menu). Same two-keys-one-action pattern as PREV.
    NEXT,
    // Enter — activate/commit the highlighted selection in the menu. In the
    // carousel it toggles Trace on RADIO and starts/cancels Probe on PROBE;
    // it remains a no-op on the other status cards. It does not open the
    // menu (BACK/ESC does that).
    SELECT,
    // Backtick/ESC — open the menu (carousel) / return to the carousel
    // (menu). Same key both opens and closes it, ui_task.cpp picks the
    // direction from current UiMode. Was Backspace-as-BACK-only; see
    // KEY_RAW_ESC_PRESS above for that first move, and PROGRESS.md's
    // 2026-08-24 bench pass for this second one (Enter-to-open felt wrong).
    BACK,
    // '1'-'5' — jump straight to that carousel page (carousel mode only).
    // Numbered to match UiPage's declared order 1:1 (ui_task.h): 1=RADIO,
    // 2=PROBE, 3=CHANNEL, 4=GPS, 5=SYSTEM. Kept as plain, separately-named
    // actions rather than one "JUMP + index" action so this header stays
    // free of any dependency on ui_task.h's UiPage enum — ui_task.cpp does
    // the index mapping itself.
    JUMP_1,
    JUMP_2,
    JUMP_3,
    JUMP_4,
    JUMP_5,
    PROBE,
    SWEEP,
};

// Maps one raw TCA8418 event byte to a KeyAction. Deliberately an allowlist:
// only the thirteen press bytes above resolve to anything — every release
// event (including these thirteen keys' own) and all other keys on the board
// return NONE. That's what keeps this safe despite covering only thirteen of
// the board's 56 keys: there's no "unknown key does something surprising"
// case, only "known key does its one thing" or "ignored."
inline KeyAction keyboardDecodeEvent(uint8_t rawEvent) {
    switch (rawEvent) {
        case KEY_RAW_COMMA_PRESS: return KeyAction::PREV;
        case KEY_RAW_SEMICOLON_PRESS: return KeyAction::PREV;
        case KEY_RAW_PERIOD_PRESS: return KeyAction::NEXT;
        case KEY_RAW_SLASH_PRESS: return KeyAction::NEXT;
        case KEY_RAW_ENTER_PRESS: return KeyAction::SELECT;
        case KEY_RAW_ESC_PRESS: return KeyAction::BACK;
        case KEY_RAW_1_PRESS: return KeyAction::JUMP_1;
        case KEY_RAW_2_PRESS: return KeyAction::JUMP_2;
        case KEY_RAW_3_PRESS: return KeyAction::JUMP_3;
        case KEY_RAW_4_PRESS: return KeyAction::JUMP_4;
        case KEY_RAW_5_PRESS: return KeyAction::JUMP_5;
        case KEY_RAW_P_PRESS: return KeyAction::PROBE;
        case KEY_RAW_S_PRESS: return KeyAction::SWEEP;
        default: return KeyAction::NONE;
    }
}
