// Guards keyboard.h's raw-byte -> KeyAction decode: the one place a wrong
// constant would either silently ignore a real keypress or, worse, make an
// unrelated key on the board fire a menu action. Runs on the host (`pio test
// -e native`), no hardware needed — see platformio.ini [env:native].
//
// The nine raw press bytes here (four nav keys + five digit-jump keys) are
// the ones keyboard.h derives and cites; this test doesn't re-derive them,
// it pins them so a future edit can't silently drift off the sourced
// values.

#include <unity.h>

#include "../../src/keyboard.h"

void test_comma_is_prev() {
    TEST_ASSERT_TRUE(KeyAction::PREV == keyboardDecodeEvent(KEY_RAW_COMMA_PRESS));
}

void test_period_is_next() {
    TEST_ASSERT_TRUE(KeyAction::NEXT == keyboardDecodeEvent(KEY_RAW_PERIOD_PRESS));
}

void test_enter_is_select() {
    TEST_ASSERT_TRUE(KeyAction::SELECT == keyboardDecodeEvent(KEY_RAW_ENTER_PRESS));
}

void test_backspace_is_back() {
    TEST_ASSERT_TRUE(KeyAction::BACK == keyboardDecodeEvent(KEY_RAW_BACKSPACE_PRESS));
}

void test_digits_are_jumps() {
    TEST_ASSERT_TRUE(KeyAction::JUMP_1 == keyboardDecodeEvent(KEY_RAW_1_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_2 == keyboardDecodeEvent(KEY_RAW_2_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_3 == keyboardDecodeEvent(KEY_RAW_3_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_4 == keyboardDecodeEvent(KEY_RAW_4_PRESS));
    TEST_ASSERT_TRUE(KeyAction::JUMP_5 == keyboardDecodeEvent(KEY_RAW_5_PRESS));
}

// Actions fire on press only. A release event is the press byte + 0x80
// (Adafruit_TCA8418::getEvent()'s own documented encoding) — must decode to
// NONE, or releasing any of these nine keys would silently repeat whatever
// its press just did.
void test_release_events_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_COMMA_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_PERIOD_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_ENTER_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_BACKSPACE_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_1_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_2_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_3_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_4_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_5_PRESS + 0x80));
}

// The allowlist property that makes covering only 9 of the board's 56 keys
// safe: everything else must resolve to NONE, not something surprising.
// These three raw values are 'q' (physical row1,col1 -> K=6), 'a' (row2,
// col2 -> K=13), and Space (row3,col13 -> K=68) — derived the same way
// keyboard.h derives its nine, from mapRawKeyToPhysical()'s formula and
// RetroBreeze's _key_value_map, picked to span different rows/cols from the
// keys actually in the allowlist (including '6'-'9'/'0', the rest of the
// digit row the five jump keys don't use).
void test_unrelated_keys_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(6));  // 'q'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(13)); // 'a'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(68)); // Space
    // The rest of the digit row, immediately adjacent to '1'-'5' in the
    // allowlist (row 0, col 6-10) — the boundary most likely to catch an
    // off-by-one in the jump keys' derivation.
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(31)); // '6'
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
    RUN_TEST(test_period_is_next);
    RUN_TEST(test_enter_is_select);
    RUN_TEST(test_backspace_is_back);
    RUN_TEST(test_digits_are_jumps);
    RUN_TEST(test_release_events_are_ignored);
    RUN_TEST(test_unrelated_keys_are_ignored);
    RUN_TEST(test_zero_is_no_event);
    return UNITY_END();
}
