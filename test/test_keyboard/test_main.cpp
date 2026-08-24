// Guards keyboard.h's raw-byte -> KeyAction decode: the one place a wrong
// constant would either silently ignore a real keypress or, worse, make an
// unrelated key on the board fire a menu action. Runs on the host (`pio test
// -e native`), no hardware needed — see platformio.ini [env:native].
//
// The four raw press bytes here are the ones keyboard.h derives and cites;
// this test doesn't re-derive them, it pins them so a future edit can't
// silently drift off the sourced values.

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

// Actions fire on press only. A release event is the press byte + 0x80
// (Adafruit_TCA8418::getEvent()'s own documented encoding) — must decode to
// NONE, or releasing any of these four keys would silently repeat whatever
// its press just did.
void test_release_events_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_COMMA_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_PERIOD_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_ENTER_PRESS + 0x80));
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(KEY_RAW_BACKSPACE_PRESS + 0x80));
}

// The allowlist property that makes covering only 4 of the board's 56 keys
// safe: everything else must resolve to NONE, not something surprising.
// These three raw values are 'q' (physical row1,col1 -> K=6), 'a' (row2,
// col2 -> K=13), and Space (row3,col13 -> K=68) — derived the same way
// keyboard.h derives its four, from mapRawKeyToPhysical()'s formula and
// RetroBreeze's _key_value_map, picked to span different rows/cols from the
// four keys actually in the allowlist.
void test_unrelated_keys_are_ignored() {
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(6));  // 'q'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(13)); // 'a'
    TEST_ASSERT_TRUE(KeyAction::NONE == keyboardDecodeEvent(68)); // Space
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
    RUN_TEST(test_release_events_are_ignored);
    RUN_TEST(test_unrelated_keys_are_ignored);
    RUN_TEST(test_zero_is_no_event);
    return UNITY_END();
}
