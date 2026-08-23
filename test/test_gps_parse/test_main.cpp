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

// Satellites IN VIEW (GSV) — the leading indicator that distinguishes
// "antenna can't see sky" from "just needs more time". Added after the
// 2026-08-23 bring-up, where `satellites` (used, from GGA) stayed 0 and
// therefore said nothing about whether the cold start was progressing.
void test_sats_in_view_sums_across_constellations() {
    GpsFix fix;
    TEST_ASSERT_EQUAL_UINT8(0, fix.sats_in_view);

    TEST_ASSERT_TRUE(gpsApplySentence(fix, "$GPGSV,3,1,11,03,03,111,00*4A", 100));
    TEST_ASSERT_EQUAL_UINT8(11, fix.sats_in_view);

    // A second constellation ADDS to the total.
    TEST_ASSERT_TRUE(gpsApplySentence(fix, "$GLGSV,1,1,04,65,20,120,30*50", 100));
    TEST_ASSERT_EQUAL_UINT8(15, fix.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(2, fix.talker_count);

    // Re-reporting the SAME constellation replaces rather than accumulates,
    // or the count would grow without bound every NMEA cycle.
    TEST_ASSERT_TRUE(gpsApplySentence(fix, "$GPGSV,3,1,11,03,03,111,00*4A", 200));
    TEST_ASSERT_EQUAL_UINT8(15, fix.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(2, fix.talker_count);
}

void test_zero_sats_in_view_is_the_indoor_signature() {
    // Verbatim from the operator's real indoor capture, 2026-08-23.
    GpsFix fix;
    gpsApplySentence(fix, "$GPGSV,1,1,00,1*64", 100);
    gpsApplySentence(fix, "$GLGSV,1,1,00,1*78", 100);
    TEST_ASSERT_EQUAL_UINT8(0, fix.sats_in_view);
    TEST_ASSERT_FALSE(fix.has_position);
}

void test_gsa_fix_type_keeps_best_then_decays_with_gga() {
    GpsFix fix;
    TEST_ASSERT_EQUAL_UINT8(1, fix.fix_type); // 1 = no fix

    TEST_ASSERT_TRUE(gpsApplySentence(fix, "$GNGSA,A,3,03,07,11,,,,,,,,,,2.1,1.2,1.7,1*33", 100));
    TEST_ASSERT_EQUAL_UINT8(3, fix.fix_type); // 3D

    // A no-fix GSA from an unused constellation must NOT clobber the 3D
    // reading — multi-GNSS receivers emit several GSA sentences per cycle.
    gpsApplySentence(fix, "$GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,2*02", 100);
    TEST_ASSERT_EQUAL_UINT8(3, fix.fix_type);

    // ...but a GGA reporting quality 0 means the fix is genuinely gone, and
    // fix_type must decay rather than latching at its best-ever value.
    gpsApplySentence(fix, "$GNGGA,181140,,,,,0,00,25.5,,,,,,*69", 200);
    TEST_ASSERT_EQUAL_UINT8(1, fix.fix_type);
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

void test_epoch_conversion_against_known_dates() {
    // Anchors checked against known Unix timestamps. This value sets the
    // device clock, which stamps every file on the card, so an off-by-a-day
    // here silently misdates a whole run's worth of evidence.
    GpsFix fix;
    fix.has_time = true;
    fix.year = 1970; fix.month = 1; fix.day = 1;
    fix.hour = 0; fix.minute = 0; fix.second = 0;
    // Refused: before GPS_MIN_PLAUSIBLE_YEAR, so it can't backdate the card.
    TEST_ASSERT_EQUAL_INT64(0, gpsFixToEpoch(fix));
    // ...but the underlying arithmetic still has the epoch itself at zero.
    TEST_ASSERT_EQUAL_INT64(0, gpsDaysFromCivil(1970, 1, 1));

    // The timestamp off the operator's own run0005 capture, 2026-08-23.
    fix.year = 2026; fix.month = 8; fix.day = 23;
    fix.hour = 15; fix.minute = 13; fix.second = 22;
    TEST_ASSERT_EQUAL_INT64(1787498002LL, gpsFixToEpoch(fix));

    // A leap-year boundary, the classic place a hand-rolled calendar breaks.
    fix.year = 2028; fix.month = 2; fix.day = 29;
    fix.hour = 0; fix.minute = 0; fix.second = 0;
    TEST_ASSERT_EQUAL_INT64(1835395200LL, gpsFixToEpoch(fix));

    // 2100 is NOT a leap year (divisible by 100, not 400).
    TEST_ASSERT_EQUAL_INT64(gpsDaysFromCivil(2100, 3, 1) - gpsDaysFromCivil(2100, 2, 28), 1);
    TEST_ASSERT_EQUAL_INT64(gpsDaysFromCivil(2028, 3, 1) - gpsDaysFromCivil(2028, 2, 28), 2);
}

void test_epoch_refuses_unusable_fixes() {
    GpsFix fix;
    // No date at all -> 0, so the clock is never set from nothing.
    TEST_ASSERT_EQUAL_INT64(0, gpsFixToEpoch(fix));

    fix.has_time = true;
    fix.year = 2026; fix.month = 8; fix.day = 23;
    fix.hour = 15; fix.minute = 13; fix.second = 22;
    TEST_ASSERT_TRUE(gpsFixToEpoch(fix) > 0);

    // Out-of-range components are refused rather than normalised: a garbled
    // sentence must not be allowed to set the system clock.
    GpsFix bad = fix; bad.month = 13;
    TEST_ASSERT_EQUAL_INT64(0, gpsFixToEpoch(bad));
    bad = fix; bad.day = 0;
    TEST_ASSERT_EQUAL_INT64(0, gpsFixToEpoch(bad));
    bad = fix; bad.hour = 24;
    TEST_ASSERT_EQUAL_INT64(0, gpsFixToEpoch(bad));
    bad = fix; bad.year = 2000; // plausible-looking, still refused
    TEST_ASSERT_EQUAL_INT64(0, gpsFixToEpoch(bad));

    // Leap second (second == 60) is accepted, not treated as corruption.
    GpsFix leap = fix; leap.second = 60;
    TEST_ASSERT_TRUE(gpsFixToEpoch(leap) > 0);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_epoch_conversion_against_known_dates);
    RUN_TEST(test_epoch_refuses_unusable_fixes);
    RUN_TEST(test_gga_with_fix_sets_position);
    RUN_TEST(test_gga_without_fix_never_sets_position);
    RUN_TEST(test_southern_western_hemispheres_are_negative);
    RUN_TEST(test_rmc_supplies_the_date);
    RUN_TEST(test_two_digit_year_windowing_is_2000s);
    RUN_TEST(test_void_rmc_clears_a_previous_position);
    RUN_TEST(test_bad_checksum_is_rejected_entirely);
    RUN_TEST(test_non_position_sentences_are_ignored_not_errors);
    RUN_TEST(test_sats_in_view_sums_across_constellations);
    RUN_TEST(test_zero_sats_in_view_is_the_indoor_signature);
    RUN_TEST(test_gsa_fix_type_keeps_best_then_decays_with_gga);
    RUN_TEST(test_fix_freshness);
    RUN_TEST(test_time_and_date_field_validation);
    return UNITY_END();
}
