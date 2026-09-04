#include <unity.h>

#include "../../src/capture_settings.h"
#include "../../src/display_settings.h"
#include "../../src/region_settings.h"
#include "../../src/sweep_margin_settings.h"

// The per-module half of the settings parsers. Until 2026-09-04 none of
// this validation had a test behind it (docs/research/
// 2026-09-04-project-audit.md, M5) — it was reachable only by booting real
// hardware with a real card, where a rejected line looks identical to a
// setting that was never changed.
//
// The contract every module shares: a line that cannot be applied returns
// false and leaves the struct untouched, so a damaged file degrades to the
// defaults rather than a half-applied mix.

void setUp(void) {}
void tearDown(void) {}

// --- capture ------------------------------------------------------------

void test_capture_accepts_every_valid_index() {
    for (uint8_t i = 0; i < CAPTURE_WINDOW_OPTION_COUNT; i++) {
        CaptureSettings s;
        char line[32];
        snprintf(line, sizeof(line), "window_index=%u", (unsigned)i);
        TEST_ASSERT_TRUE(applyCaptureConfigLine(line, s));
        TEST_ASSERT_EQUAL_UINT8(i, s.window_index);
    }
}

void test_capture_rejects_out_of_range_index() {
    CaptureSettings s;
    const uint8_t original = s.window_index;
    TEST_ASSERT_FALSE(applyCaptureConfigLine("window_index=4", s));
    TEST_ASSERT_FALSE(applyCaptureConfigLine("window_index=99", s));
    TEST_ASSERT_FALSE(applyCaptureConfigLine("window_index=-1", s));
    TEST_ASSERT_EQUAL_UINT8(original, s.window_index);
}

// Regression for a real defect this refactor fixed: String::toInt() mapped
// unparseable text to 0, and 0 is a *valid* window index (OFF) — so a
// corrupt line silently disabled packet capture instead of being ignored.
void test_capture_garbage_does_not_silently_select_off() {
    CaptureSettings s;
    s.window_index = 2; // 2s, the shipped default
    TEST_ASSERT_FALSE(applyCaptureConfigLine("window_index=garbage", s));
    TEST_ASSERT_FALSE(applyCaptureConfigLine("window_index=", s));
    TEST_ASSERT_FALSE(applyCaptureConfigLine("window_index=0x0", s));
    TEST_ASSERT_EQUAL_UINT8(2, s.window_index); // NOT 0/OFF
}

void test_capture_ignores_unrelated_keys_and_comments() {
    CaptureSettings s;
    TEST_ASSERT_FALSE(applyCaptureConfigLine("# window_index=0", s));
    TEST_ASSERT_FALSE(applyCaptureConfigLine("margin_dbm_x10=350", s));
    TEST_ASSERT_FALSE(applyCaptureConfigLine("", s));
    TEST_ASSERT_EQUAL_UINT8(CAPTURE_WINDOW_DEFAULT_INDEX, s.window_index);
}

// --- sweep margin -------------------------------------------------------

void test_margin_accepts_bounds_and_rejects_beyond() {
    SweepMarginSettings s;
    TEST_ASSERT_TRUE(applySweepMarginConfigLine("margin_dbm_x10=150", s));
    TEST_ASSERT_EQUAL_INT16(ENERGY_SWEEP_MARGIN_MIN_DBM_X10, s.margin_dbm_x10);
    TEST_ASSERT_TRUE(applySweepMarginConfigLine("margin_dbm_x10=500", s));
    TEST_ASSERT_EQUAL_INT16(ENERGY_SWEEP_MARGIN_MAX_DBM_X10, s.margin_dbm_x10);
    TEST_ASSERT_TRUE(applySweepMarginConfigLine("margin_dbm_x10=350", s));
    TEST_ASSERT_EQUAL_INT16(350, s.margin_dbm_x10);

    const int16_t keep = s.margin_dbm_x10;
    TEST_ASSERT_FALSE(applySweepMarginConfigLine("margin_dbm_x10=149", s));
    TEST_ASSERT_FALSE(applySweepMarginConfigLine("margin_dbm_x10=501", s));
    TEST_ASSERT_FALSE(applySweepMarginConfigLine("margin_dbm_x10=0", s));
    TEST_ASSERT_FALSE(applySweepMarginConfigLine("margin_dbm_x10=garbage", s));
    TEST_ASSERT_EQUAL_INT16(keep, s.margin_dbm_x10);
}

// --- region -------------------------------------------------------------

void test_region_accepts_both_tokens() {
    RegionSettings s;
    TEST_ASSERT_TRUE(applyRegionConfigLine("region=GLOBAL", s));
    TEST_ASSERT_TRUE(Region::GLOBAL == s.region);
    TEST_ASSERT_TRUE(applyRegionConfigLine("region=US", s));
    TEST_ASSERT_TRUE(Region::US == s.region);
}

// A typo'd region must not fall back to a default silently — it would scan
// the wrong band and look like a hardware problem.
void test_region_rejects_unknown_or_miscased_tokens() {
    RegionSettings s;
    s.region = Region::GLOBAL;
    TEST_ASSERT_FALSE(applyRegionConfigLine("region=us", s));
    TEST_ASSERT_FALSE(applyRegionConfigLine("region=EU", s));
    TEST_ASSERT_FALSE(applyRegionConfigLine("region=", s));
    TEST_ASSERT_FALSE(applyRegionConfigLine("region=US ExtraJunk", s));
    TEST_ASSERT_TRUE(Region::GLOBAL == s.region);
}

// --- display ------------------------------------------------------------

void test_display_accepts_on_step_brightness_only() {
    DisplaySettings s;
    TEST_ASSERT_TRUE(applyDisplayConfigLine("brightness_pct=5", s));
    TEST_ASSERT_EQUAL_UINT8(5, s.brightness_pct);
    TEST_ASSERT_TRUE(applyDisplayConfigLine("brightness_pct=100", s));
    TEST_ASSERT_EQUAL_UINT8(100, s.brightness_pct);

    TEST_ASSERT_FALSE(applyDisplayConfigLine("brightness_pct=7", s));   // off-step
    TEST_ASSERT_FALSE(applyDisplayConfigLine("brightness_pct=0", s));   // below min
    TEST_ASSERT_FALSE(applyDisplayConfigLine("brightness_pct=105", s)); // above max
    TEST_ASSERT_EQUAL_UINT8(100, s.brightness_pct);
}

void test_display_idle_timeout_index_bounds() {
    DisplaySettings s;
    for (uint8_t i = 0; i <= IDLE_TIMEOUT_INDEX_MAX; i++) {
        char line[40];
        snprintf(line, sizeof(line), "idle_timeout_index=%u", (unsigned)i);
        TEST_ASSERT_TRUE(applyDisplayConfigLine(line, s));
        TEST_ASSERT_EQUAL_UINT8(i, s.idle_timeout_index);
    }
    s.idle_timeout_index = 2;
    TEST_ASSERT_FALSE(applyDisplayConfigLine("idle_timeout_index=5", s));
    // Same toInt()-returns-0 trap as capture's: 0 is a valid index (Off).
    TEST_ASSERT_FALSE(applyDisplayConfigLine("idle_timeout_index=garbage", s));
    TEST_ASSERT_EQUAL_UINT8(2, s.idle_timeout_index);
}

// A real file is a mix of comments, blanks and values; applying it should
// leave exactly the valid settings and ignore the rest.
void test_display_whole_file_shape() {
    DisplaySettings s;
    const char *file[] = {
        "# LoRaTrace RX - display settings",
        "# brightness_pct: 5-100 in 5% steps",
        "",
        "brightness_pct=45",
        "idle_timeout_index=3",
        "bogus_key=1",
        "   ",
    };
    int applied = 0;
    for (const char *line : file) {
        if (applyDisplayConfigLine(line, s)) applied++;
    }
    TEST_ASSERT_EQUAL_INT(2, applied);
    TEST_ASSERT_EQUAL_UINT8(45, s.brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(3, s.idle_timeout_index);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_capture_accepts_every_valid_index);
    RUN_TEST(test_capture_rejects_out_of_range_index);
    RUN_TEST(test_capture_garbage_does_not_silently_select_off);
    RUN_TEST(test_capture_ignores_unrelated_keys_and_comments);
    RUN_TEST(test_margin_accepts_bounds_and_rejects_beyond);
    RUN_TEST(test_region_accepts_both_tokens);
    RUN_TEST(test_region_rejects_unknown_or_miscased_tokens);
    RUN_TEST(test_display_accepts_on_step_brightness_only);
    RUN_TEST(test_display_idle_timeout_index_bounds);
    RUN_TEST(test_display_whole_file_shape);
    return UNITY_END();
}
