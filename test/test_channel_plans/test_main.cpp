// Guards the one class of mistake DESIGN.md/CLAUDE.md worry about most for
// this file: a typo'd RF constant that's wrong but not obviously wrong
// (e.g. 906.875 -> 960.875, SF11 -> SF1). Runs on the host (`pio test -e
// native`), no hardware needed — see platformio.ini [env:native].

#include <unity.h>

#include "../../src/channel_plans.h"

void test_meshtastic_longfast_in_tuned_band() {
    // Module front end is tuned 868-923MHz (DESIGN.md §1).
    TEST_ASSERT_TRUE(CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz >= 868.0f);
    TEST_ASSERT_TRUE(CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz <= 923.0f);
    TEST_ASSERT_EQUAL_UINT8(11, CHANNEL_MESHTASTIC_LONGFAST_US.sf);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, CHANNEL_MESHTASTIC_LONGFAST_US.bw_khz);
    TEST_ASSERT_EQUAL_UINT8(8, CHANNEL_MESHTASTIC_LONGFAST_US.cr_denom);
}

void test_meshcore_narrow_in_tuned_band() {
    TEST_ASSERT_TRUE(CHANNEL_MESHCORE_US_NARROW.freq_mhz >= 868.0f);
    TEST_ASSERT_TRUE(CHANNEL_MESHCORE_US_NARROW.freq_mhz <= 923.0f);
    TEST_ASSERT_EQUAL_UINT8(7, CHANNEL_MESHCORE_US_NARROW.sf);
    TEST_ASSERT_EQUAL_FLOAT(62.5f, CHANNEL_MESHCORE_US_NARROW.bw_khz);
    TEST_ASSERT_EQUAL_UINT8(5, CHANNEL_MESHCORE_US_NARROW.cr_denom);
}

void test_meshtastic_and_meshcore_dont_collide() {
    // Different enough that a HOME_LISTEN lock on one won't pick up the
    // other by accident.
    float delta = CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz - CHANNEL_MESHCORE_US_NARROW.freq_mhz;
    if (delta < 0) delta = -delta;
    TEST_ASSERT_TRUE(delta > 1.0f);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_meshtastic_longfast_in_tuned_band);
    RUN_TEST(test_meshcore_narrow_in_tuned_band);
    RUN_TEST(test_meshtastic_and_meshcore_dont_collide);
    return UNITY_END();
}
