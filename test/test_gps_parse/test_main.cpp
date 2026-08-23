// Guards GPS parsing — the half of Phase 2 that decides whether a detection
// is attributed to the right place on the map. A wrong coordinate is worse
// than a missing one, so most of these tests are about REFUSING to produce a
// position rather than producing one.

#include <unity.h>

#include "../../src/gps_parse.h"

// All fixtures carry real, correct NMEA checksums.
static const char *GGA_FIX = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
static const char *GGA_NOFIX = "$GPGGA,123519,,,,,0,00,,,M,,M,,*6B";
static const char *RMC_ACTIVE =
    "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
static const char *RMC_VOID = "$GPRMC,123519,V,,,,,,,230394,,*33";
// Multi-GNSS talker ID (GN*), which the ATGM336H on this board emits once it
// sees more than one constellation. Matching only "GP" would ignore these.
static const char *GNGGA_FIX =
    "$GNGGA,181140,4519.5000,N,12240.2500,W,1,09,0.9,50.0,M,0.0,M,,*48";
static const char *GNRMC_FIX =
    "$GNRMC,181140,A,4519.5000,N,12240.2500,W,0.0,0.0,230826,,,A*72";

void test_gga_with_fix_sets_position() {
    GpsFix fix;
    TEST_ASSERT_TRUE(gpsApplySentence(fix, GGA_FIX, 1000));
    TEST_ASSERT_TRUE(fix.has_position);
    TEST_ASSERT_EQUAL_UINT8(1, fix.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(8, fix.satellites);
    // 4807.038 N -> 48 + 7.038/60
    TEST_ASSERT_FLOAT_WITHIN(0.0005, 48.1173, fix.lat);
    TEST_ASSERT_FLOAT_WITHIN(0.0005, 11.5167, fix.lon);
    TEST_ASSERT_EQUAL_UINT32(1000, fix.updated_ms);
    TEST_ASSERT_EQUAL_UINT8(12, fix.hour);
    TEST_ASSERT_EQUAL_UINT8(35, fix.minute);
    TEST_ASSERT_EQUAL_UINT8(19, fix.second);
}

void test_gga_without_fix_never_sets_position() {
    GpsFix fix;
    gpsApplySentence(fix, GGA_NOFIX, 1000);
    TEST_ASSERT_FALSE(fix.has_position);
    TEST_ASSERT_EQUAL_UINT8(0, fix.fix_quality);
    // Must remain exactly 0 and unusable, not "0,0 which looks like a place"
    TEST_ASSERT_EQUAL_UINT32(0, fix.updated_ms);
}

void test_southern_western_hemispheres_are_negative() {
    GpsFix fix;
    TEST_ASSERT_TRUE(gpsApplySentence(fix, GNGGA_FIX, 500));
    TEST_ASSERT_TRUE(fix.has_position);
    TEST_ASSERT_TRUE(fix.lat > 0);  // N
    TEST_ASSERT_TRUE(fix.lon < 0);  // W  <- the sign bug that would put the
                                    //       Pacific Northwest in China
    TEST_ASSERT_FLOAT_WITHIN(0.001, 45.325, fix.lat);
    TEST_ASSERT_FLOAT_WITHIN(0.001, -122.6708, fix.lon);
}

void test_rmc_supplies_the_date() {
    GpsFix fix;
    TEST_ASSERT_TRUE(gpsApplySentence(fix, RMC_ACTIVE, 100));
    TEST_ASSERT_TRUE(fix.has_time);
    TEST_ASSERT_EQUAL_UINT8(3, fix.month); // 230394 -> 23 Mar
    TEST_ASSERT_EQUAL_UINT8(23, fix.day);
    TEST_ASSERT_TRUE(fix.has_position);

    GpsFix modern;
    gpsApplySentence(modern, GNRMC_FIX, 100);
    TEST_ASSERT_EQUAL_UINT16(2026, modern.year); // 230826 -> 23 Aug 2026
}

void test_two_digit_year_windowing_is_2000s() {
    // NMEA only carries two digits, so some century rule is unavoidable.
    // gps_parse.h windows to 2000-2099 deliberately. The canonical NMEA
    // example sentence dates from 1994 and therefore reads as 2094 here —
    // asserted explicitly so the rule is visible rather than surprising
    // someone debugging a timestamp later.
    uint16_t y;
    uint8_t mo, d;
    TEST_ASSERT_TRUE(gpsParseDateField("230394", &y, &mo, &d));
    TEST_ASSERT_EQUAL_UINT16(2094, y);
    TEST_ASSERT_TRUE(gpsParseDateField("010100", &y, &mo, &d));
    TEST_ASSERT_EQUAL_UINT16(2000, y);
    TEST_ASSERT_TRUE(gpsParseDateField("311299", &y, &mo, &d));
    TEST_ASSERT_EQUAL_UINT16(2099, y);
}

void test_void_rmc_clears_a_previous_position() {
    // Losing the fix must drop the position, not silently keep attributing
    // new detections to where we last were.
    GpsFix fix;
    gpsApplySentence(fix, GGA_FIX, 1000);
    TEST_ASSERT_TRUE(fix.has_position);

    gpsApplySentence(fix, RMC_VOID, 2000);
    TEST_ASSERT_FALSE(fix.has_position);
}

void test_bad_checksum_is_rejected_entirely() {
    GpsFix fix;
    // Same sentence, deliberately corrupted checksum.
    TEST_ASSERT_FALSE(gpsApplySentence(
        fix, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00", 1000));
    TEST_ASSERT_FALSE(fix.has_position);
    // A sentence with no checksum at all is equally untrusted.
    TEST_ASSERT_FALSE(gpsApplySentence(fix, "$GPGGA,123519,4807.038,N,01131.000,E,1,08", 1000));
}

void test_non_position_sentences_are_ignored_not_errors() {
    GpsFix fix;
    // GSV is valid NMEA the module emits constantly; it just isn't useful.
    TEST_ASSERT_FALSE(gpsApplySentence(fix, "$GPGSV,3,1,11,03,03,111,00*4B", 1000));
    TEST_ASSERT_FALSE(fix.has_position);
    TEST_ASSERT_FALSE(gpsApplySentence(fix, nullptr, 1000));
    TEST_ASSERT_FALSE(gpsApplySentence(fix, "garbage", 1000));
}

void test_fix_freshness() {
    GpsFix fix;
    gpsApplySentence(fix, GGA_FIX, 10000);
    TEST_ASSERT_TRUE(gpsFixIsFresh(fix, 12000, 10000));  // 2s old, limit 10s
    TEST_ASSERT_FALSE(gpsFixIsFresh(fix, 30000, 10000)); // 20s old, limit 10s

    GpsFix never;
    TEST_ASSERT_FALSE(gpsFixIsFresh(never, 1000, 10000));
}

void test_time_and_date_field_validation() {
    uint8_t h, m, s;
    TEST_ASSERT_TRUE(gpsParseTimeField("235959", &h, &m, &s));
    TEST_ASSERT_EQUAL_UINT8(23, h);
    TEST_ASSERT_TRUE(gpsParseTimeField("120000.500", &h, &m, &s)); // fractional ok
    TEST_ASSERT_FALSE(gpsParseTimeField("245959", &h, &m, &s));    // hour 24
    TEST_ASSERT_FALSE(gpsParseTimeField("12345", &h, &m, &s));     // too short
    TEST_ASSERT_FALSE(gpsParseTimeField("12ab59", &h, &m, &s));    // non-numeric
    TEST_ASSERT_FALSE(gpsParseTimeField(nullptr, &h, &m, &s));

    uint16_t y;
    uint8_t mo, d;
    TEST_ASSERT_TRUE(gpsParseDateField("230826", &y, &mo, &d));
    TEST_ASSERT_FALSE(gpsParseDateField("231326", &y, &mo, &d)); // month 13
    TEST_ASSERT_FALSE(gpsParseDateField("000826", &y, &mo, &d)); // day 0
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_gga_with_fix_sets_position);
    RUN_TEST(test_gga_without_fix_never_sets_position);
    RUN_TEST(test_southern_western_hemispheres_are_negative);
    RUN_TEST(test_rmc_supplies_the_date);
    RUN_TEST(test_two_digit_year_windowing_is_2000s);
    RUN_TEST(test_void_rmc_clears_a_previous_position);
    RUN_TEST(test_bad_checksum_is_rejected_entirely);
    RUN_TEST(test_non_position_sentences_are_ignored_not_errors);
    RUN_TEST(test_fix_freshness);
    RUN_TEST(test_time_and_date_field_validation);
    return UNITY_END();
}
