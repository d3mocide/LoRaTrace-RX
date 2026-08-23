// Guards the battery percentage curve. Pure math, but worth pinning: the
// constants come from M5Unified (see battery.h) and their slight asymmetry
// — subtract 3300, divide by (4150-3350) — looks like a typo to anyone
// reading it fresh. It isn't; it's reproduced deliberately so readings match
// every other Cardputer firmware. A well-meaning "fix" would silently shift
// every reading, and these tests are what should stop it.

#include <unity.h>

#include "../../src/battery.h"

void test_endpoints_and_clamping() {
    TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMv(3300));

    // Above the curve's top must clamp to 100, never overflow past it.
    TEST_ASSERT_EQUAL_UINT8(100, batteryPercentFromMv(4200));
    TEST_ASSERT_EQUAL_UINT8(100, batteryPercentFromMv(5000));

    // Below empty clamps to 0 rather than going negative — the function
    // returns unsigned, so an unclamped subtraction would wrap to ~255%.
    TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMv(3000));
    TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMv(0));
}

void test_curve_is_monotonic_and_sane() {
    // (mv - 3300) * 100 / 800
    TEST_ASSERT_EQUAL_UINT8(50, batteryPercentFromMv(3700));
    TEST_ASSERT_EQUAL_UINT8(25, batteryPercentFromMv(3500));
    TEST_ASSERT_EQUAL_UINT8(75, batteryPercentFromMv(3900));

    uint8_t prev = 0;
    for (uint32_t mv = 3300; mv <= 4200; mv += 25) {
        uint8_t pct = batteryPercentFromMv(mv);
        TEST_ASSERT_TRUE(pct >= prev); // must never go down as voltage rises
        prev = pct;
    }
    TEST_ASSERT_EQUAL_UINT8(100, prev);
}

void test_divider_ratio_is_two() {
    // The ADC sees half the battery voltage. If this constant is ever
    // "simplified" away, every reading halves and the device reports a flat
    // battery permanently.
    TEST_ASSERT_EQUAL_FLOAT(2.0f, BATTERY_ADC_RATIO);
    TEST_ASSERT_EQUAL_INT8(10, PIN_BATTERY_ADC);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_endpoints_and_clamping);
    RUN_TEST(test_curve_is_monotonic_and_sane);
    RUN_TEST(test_divider_ratio_is_two);
    return UNITY_END();
}
