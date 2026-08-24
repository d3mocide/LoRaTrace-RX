#pragma once
// LoRaTrace RX — keyboard input decode (Phase 5).
//
// Turns a raw TCA8418 event byte (Adafruit_TCA8418::getEvent()) into one of
// four navigation actions, for the on-device menu (ui_task.cpp). Pure logic,
// no Arduino dependency, so it's host-testable (pio test -e native) same as
// detection.h/gps_parse.h.
//
// Deliberately NOT a general keymap. CLAUDE.md's house rule is not to guess
// hardware tables, and until now this project had no sourced Cardputer-ADV
// row/col-to-character map — every past on-device gesture (ui_task.cpp's now
// -removed KeyGesture/pollKeyGesture()) was built on undifferentiated
// press/release timing specifically to avoid needing one. Phase 5's UX
// decision (plain ','/'.' to move, Enter to select, Backspace to go back —
// no Fn chord, no numeric entry) only ever needs FOUR specific keys
// identified, not the full 56-key table, which is what makes a real,
// sourced decode tractable as a small, low-risk slice instead of a project
// of its own.
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
//
// Inverting step 2's formula for exactly these four (row, col) pairs
// (t = col>>1, topbit = col&1, u = ((topbit<<2)|row)+1, K = t*10+u; checked
// by plugging each result back into the forward formula above) gives the
// four raw press-byte constants below. Nothing here has been bench-verified
// yet — same bar as every other sourced-not-measured hardware fact in this
// project (sync words, IO-expander registers, TFT offsets all needed a real
// boot before being trusted): press each of these four keys once on real
// hardware and confirm the firmware recognizes exactly that key, per
// PROGRESS.md's Phase 5 checklist.

#include <stdint.h>

// Backspace: physical (row 0, col 13) -> t=6, u=5 -> K=65.
constexpr uint8_t KEY_RAW_BACKSPACE_PRESS = 65;
// Enter: physical (row 2, col 13) -> t=6, u=7 -> K=67.
constexpr uint8_t KEY_RAW_ENTER_PRESS = 67;
// Comma ',': physical (row 3, col 10) -> t=5, u=4 -> K=54.
constexpr uint8_t KEY_RAW_COMMA_PRESS = 54;
// Period '.': physical (row 3, col 11) -> t=5, u=8 -> K=58.
constexpr uint8_t KEY_RAW_PERIOD_PRESS = 58;

enum class KeyAction {
    NONE,
    PREV,   // ',' — previous status page (carousel) / move selection up (menu)
    NEXT,   // '.' — next status page (carousel) / move selection down (menu)
    SELECT, // Enter — open the menu (carousel) / activate selection (menu)
    BACK,   // Backspace — return to the carousel (menu only; no-op in carousel)
};

// Maps one raw TCA8418 event byte to a KeyAction. Deliberately an allowlist:
// only the four press bytes above resolve to anything — every release event
// (including these four keys' own) and all 52 other keys on the board return
// NONE. That's what keeps this safe despite covering only four of the
// board's 56 keys: there's no "unknown key does something surprising" case,
// only "known key does its one thing" or "ignored."
inline KeyAction keyboardDecodeEvent(uint8_t rawEvent) {
    switch (rawEvent) {
        case KEY_RAW_COMMA_PRESS: return KeyAction::PREV;
        case KEY_RAW_PERIOD_PRESS: return KeyAction::NEXT;
        case KEY_RAW_ENTER_PRESS: return KeyAction::SELECT;
        case KEY_RAW_BACKSPACE_PRESS: return KeyAction::BACK;
        default: return KeyAction::NONE;
    }
}
