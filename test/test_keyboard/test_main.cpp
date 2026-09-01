// Guards keyboard.h's raw-byte -> KeyAction decode: the one place a wrong
// constant would either silently ignore a real keypress or, worse, make an
// unrelated key on the board fire a menu action. Runs on the host (`pio test
// -e native`), no hardware needed — see platformio.ini [env:native].
//
// The seventeen raw press bytes here (Enter, Comma/Semicolon, Period/Slash,
// backtick/ESC, seven digit-jump keys, P/Probe, S/Sweep, R/repeat, and
// C/Cell) are the ones keyboard.h derives and cites; this test doesn't re-derive
// them, it pins them so a future edit can't silently drift off the sourced
// values. It also round-trips each one through keyboardPhysicalPosition() —
// pinning a constant against itself is what let the Ctrl+S modifier chord
// ship green three times before real hardware testing (KEY_DUMP) caught it
// dropping its own release event and it was reverted in favor of a
// dedicated key (R) — see keyboard.h's top-of-file note.

#include <unity.h>

#include "../../src/keyboard.h"

void test_comma_is_prev() {
    TEST_ASSERT_TRUE(KeyAction::PREV == keyboardDecodeEvent(KEY_RAW_COMMA_PRESS));
}

// Semicolon is the Fn-arrow diamond's "up" key, an alias for the same PREV
// the carousel/menu already use via Comma ("left") — added after bench
// testing found it didn't do anything, unlike Comma/Period which already
// worked by virtue of being in the original four.
void test_semicolon_is_prev() {
    TEST_ASSERT_TRUE(KeyAction::PREV == keyboardDecodeEvent(KEY_RAW_SEMICOLON_PRESS));
}

void test_period_is_next() {
    TEST_ASSERT_TRUE(KeyAction::NEXT == keyboardDecodeEvent(KEY_RAW_PERIOD_PRESS));
}

// Slash is the Fn-arrow diamond's "right" key, an alias for the same NEXT
// Period ("down") already triggers.
void test_slash_is_next() {
    TEST_ASSERT_TRUE(KeyAction::NEXT == keyboardDecodeEvent(KEY_RAW_SLASH_PRESS));
}

void test_enter_is_select() {
    TEST_ASSERT_TRUE(KeyAction::SELECT == keyboardDecodeEvent(KEY_RAW_ENTER_PRESS));
}

void test_esc_is_back() {
    TEST_ASSERT_TRUE(KeyAction::BACK == keyboardDecodeEvent(KEY_RAW_ESC_PRESS));
}

// Backspace used to be BACK; bench testing found it felt wrong and it was
// swapped for the backtick/ESC key above. Pinned here as a regression
// check — Backspace must go back to being just an ordinary ignored key,
// not silently keep working via a leftover case.
void test_backspace_is_no_longer_mapped() {
    constexpr uint8_t KEY_RAW_BACKSPACE_PRESS = 65; // (row 0, col 13)
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_BACKSPACE_PRESS));
}

void test_digits_are_jumps() {
    TEST_ASSERT_TRUE(KeyAction::JUMP_1 == keyboardDecodeEvent(KEY_RAW_1_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_2 == keyboardDecodeEvent(KEY_RAW_2_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_3 == keyboardDecodeEvent(KEY_RAW_3_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_4 == keyboardDecodeEvent(KEY_RAW_4_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_5 == keyboardDecodeEvent(KEY_RAW_5_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_6 == keyboardDecodeEvent(KEY_RAW_6_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_7 == keyboardDecodeEvent(KEY_RAW_7_PRESS));
}

void test_p_is_probe_shortcut() {
    TEST_ASSERT_TRUE(KeyAction::PROBE == keyboardDecodeEvent(KEY_RAW_P_PRESS));
}

void test_s_is_sweep_shortcut() {
    TEST_ASSERT_TRUE(KeyAction::SWEEP == keyboardDecodeEvent(KEY_RAW_S_PRESS));
}

// R is the repeat toggle, a dedicated key rather than a Ctrl+S modifier
// chord (reverted 2026-08-30 — see keyboard.h's top-of-file note). This
// layer only decodes the raw byte to a KeyAction; ui_task.cpp is what gates
// it to the Sweep/Cell cards, so the mapping here stays page-agnostic.
void test_r_is_repeat_shortcut() {
    TEST_ASSERT_TRUE(KeyAction::REPEAT == keyboardDecodeEvent(KEY_RAW_R_PRESS));
}

// C is the global Cell shortcut (Phase 11, 2026-09-01) — same flat
// key-to-KeyAction shape as P/S/R above (what ui_task.cpp does with each
// action afterward differs: P/S/C stay global, R is page-gated).
void test_c_is_cell_shortcut() {
    TEST_ASSERT_TRUE(KeyAction::CELL == keyboardDecodeEvent(KEY_RAW_C_PRESS));
}

// --- The derivation itself ---
//
// Every constant in keyboard.h was produced by inverting Launcher's
// mapRawKeyToPhysical() by hand, and this round-trips each one through the
// forward formula so it's checked against the (row, col) its own comment
// claims, not just pinned against itself.
static void assertPosition(uint8_t keyNumber, uint8_t expectRow, uint8_t expectCol) {
    uint8_t row = 0;
    uint8_t col = 0;
    TEST_ASSERT_TRUE(keyboardPhysicalPosition(keyNumber, row, col));
    TEST_ASSERT_EQUAL_UINT8(expectRow, row);
    TEST_ASSERT_EQUAL_UINT8(expectCol, col);
}

void test_every_mapped_key_round_trips_to_its_documented_position() {
    assertPosition(KEY_RAW_ESC_PRESS, 0, 0);
    assertPosition(KEY_RAW_1_PRESS, 0, 1);
    assertPosition(KEY_RAW_2_PRESS, 0, 2);
    assertPosition(KEY_RAW_3_PRESS, 0, 3);
    assertPosition(KEY_RAW_4_PRESS, 0, 4);
    assertPosition(KEY_RAW_5_PRESS, 0, 5);
    assertPosition(KEY_RAW_6_PRESS, 0, 6);
    assertPosition(KEY_RAW_7_PRESS, 0, 7);
    assertPosition(KEY_RAW_R_PRESS, 1, 4);
    assertPosition(KEY_RAW_P_PRESS, 1, 10);
    assertPosition(KEY_RAW_S_PRESS, 2, 3);
    assertPosition(KEY_RAW_SEMICOLON_PRESS, 2, 11);
    assertPosition(KEY_RAW_ENTER_PRESS, 2, 13);
    assertPosition(KEY_RAW_C_PRESS, 3, 5);
    assertPosition(KEY_RAW_COMMA_PRESS, 3, 10);
    assertPosition(KEY_RAW_PERIOD_PRESS, 3, 11);
    assertPosition(KEY_RAW_SLASH_PRESS, 3, 12);
}

// The formula's stated domain (u in 1..8, t <= 6) is the ADV's 7x8 scan
// range; key numbers outside it are not positions on this keyboard and must
// be rejected rather than silently producing a plausible-looking (row, col).
void test_out_of_range_key_numbers_are_rejected() {
    uint8_t row = 0;
    uint8_t col = 0;
    TEST_ASSERT_FALSE(keyboardPhysicalPosition(0, row, col));  // "no event"
    TEST_ASSERT_FALSE(keyboardPhysicalPosition(9, row, col));  // u = 9
    TEST_ASSERT_FALSE(keyboardPhysicalPosition(10, row, col)); // u = 0
    TEST_ASSERT_FALSE(keyboardPhysicalPosition(71, row, col)); // t = 7
}

// keyboardEventIsRelease()/keyboardEventKeyNumber() back ui_task.cpp's
// KEY_DUMP diagnostic — pinned directly since nothing else here exercises
// them (keyboardDecodeEvent() only ever sees full raw bytes, never needs to
// split them).
void test_event_release_flag_and_key_number_split_correctly() {
    TEST_ASSERT_FALSE(keyboardEventIsRelease(KEY_RAW_S_PRESS));
    TEST_ASSERT_TRUE(keyboardEventIsRelease(KEY_RAW_S_PRESS | KEY_RAW_RELEASE_FLAG));
    TEST_ASSERT_EQUAL_UINT8(KEY_RAW_S_PRESS, keyboardEventKeyNumber(KEY_RAW_S_PRESS));
    TEST_ASSERT_EQUAL_UINT8(KEY_RAW_S_PRESS,
                            keyboardEventKeyNumber(KEY_RAW_S_PRESS | KEY_RAW_RELEASE_FLAG));
}

// Actions fire on press only. A release event is the press byte + 0x80
// (Adafruit_TCA8418::getEvent()'s own documented encoding) — must decode to
// NONE, or releasing any of these keys would silently repeat whatever its
// press just did.
void test_release_events_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_COMMA_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_SEMICOLON_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_PERIOD_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_SLASH_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_ENTER_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_ESC_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_1_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_2_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_3_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_4_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_5_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_6_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_7_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_P_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_S_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_R_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_C_PRESS + 0x80));
}

// The allowlist property that makes covering only 15 of the board's 56 keys
// safe: everything else must resolve to NONE, not something surprising.
// These three raw values are 'q' (physical row1,col1 -> K=6, also
// independently confirmed by kamrrillo/Cardputer-ADV-Keyboard's own
// pressed-key capture), 'a' (row2, col2 -> K=13), and Space (row3, col13 ->
// K=68) — picked to span different rows/cols from the keys actually in the
// allowlist.
void test_unrelated_keys_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(6));  // 'q'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(13)); // 'a'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(68)); // Space
    // The rest of the digit row, immediately adjacent to '1'-'7' in the
    // allowlist (row 0, col 8-10; col 7/'7' moved INTO the allowlist as
    // JUMP_7, Phase 11 — see test_digits_are_jumps()) — the boundary most
    // likely to catch an off-by-one in the jump keys' derivation.
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(41)); // '8'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(45)); // '9'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(51)); // '0'
    // Row 1 letters adjacent to R (col4) and P (col10) — the boundary most
    // likely to catch an off-by-one in R's derivation.
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(12)); // 'w' (col2)
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(16)); // 'e' (col3)
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(26)); // 't' (col5)
    // Row 3 letters adjacent to C (col5) — the boundary most likely to catch
    // an off-by-one in C's derivation (Phase 11).
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(24)); // 'x' (col4)
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(34)); // 'v' (col6)
}

void test_zero_is_no_event() {
    // 0x00 is TCA8418's own "no event" value (Adafruit_TCA8418::getEvent()).
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(0));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_comma_is_prev);
    RUN_TEST(test_semicolon_is_prev);
    RUN_TEST(test_period_is_next);
    RUN_TEST(test_slash_is_next);
    RUN_TEST(test_enter_is_select);
    RUN_TEST(test_esc_is_back);
    RUN_TEST(test_backspace_is_no_longer_mapped);
    RUN_TEST(test_digits_are_jumps);
    RUN_TEST(test_p_is_probe_shortcut);
    RUN_TEST(test_s_is_sweep_shortcut);
    RUN_TEST(test_r_is_repeat_shortcut);
    RUN_TEST(test_c_is_cell_shortcut);
    RUN_TEST(test_every_mapped_key_round_trips_to_its_documented_position);
    RUN_TEST(test_out_of_range_key_numbers_are_rejected);
    RUN_TEST(test_event_release_flag_and_key_number_split_correctly);
    RUN_TEST(test_release_events_are_ignored);
    RUN_TEST(test_unrelated_keys_are_ignored);
    RUN_TEST(test_zero_is_no_event);
    return UNITY_END();
}
