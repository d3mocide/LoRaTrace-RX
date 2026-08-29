// Guards keyboard.h's raw-byte -> KeyAction decode: the one place a wrong
// constant would either silently ignore a real keypress or, worse, make an
// unrelated key on the board fire a menu action. Runs on the host (`pio test
// -e native`), no hardware needed — see platformio.ini [env:native].
//
// The fourteen raw press bytes here (Enter, Comma/Semicolon, Period/Slash,
// backtick/ESC, six digit-jump keys, P/Probe, and S/Sweep) are the ones
// keyboard.h derives and cites; this test doesn't re-derive them, it pins
// them so a future edit can't silently drift off the sourced values.

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
}

void test_p_is_probe_shortcut() {
    TEST_ASSERT_TRUE(KeyAction::PROBE == keyboardDecodeEvent(KEY_RAW_P_PRESS));
}

void test_s_is_sweep_shortcut() {
    TEST_ASSERT_TRUE(KeyAction::SWEEP == keyboardDecodeEvent(KEY_RAW_S_PRESS));
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
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_P_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_S_PRESS + 0x80));
}

// The allowlist property that makes covering only 14 of the board's 56 keys
// safe: everything else must resolve to NONE, not something surprising.
// These three raw values are 'q' (physical row1,col1 -> K=6), 'a' (row2,
// col2 -> K=13), and Space (row3,col13 -> K=68) — derived the same way
// keyboard.h derives its own keys, from mapRawKeyToPhysical()'s formula and
// RetroBreeze's _key_value_map, picked to span different rows/cols from the
// keys actually in the allowlist ('7'-'9'/'0', the rest of the digit row
// the six jump keys don't use — '6' itself moved to the allowlist above).
void test_unrelated_keys_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(6));  // 'q'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(13)); // 'a'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(68)); // Space
    // The rest of the digit row, immediately adjacent to '1'-'6' in the
    // allowlist (row 0, col 7-10) — the boundary most likely to catch an
    // off-by-one in the jump keys' derivation.
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(35)); // '7'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(41)); // '8'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(45)); // '9'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(51)); // '0'
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
    RUN_TEST(test_release_events_are_ignored);
    RUN_TEST(test_unrelated_keys_are_ignored);
    RUN_TEST(test_zero_is_no_event);
    return UNITY_END();
}
