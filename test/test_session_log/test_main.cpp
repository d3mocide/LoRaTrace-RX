// Guards the session-health row (src/session_log.h).
//
// This file is instrumentation for an unattended run, which makes it
// exactly the kind of code that fails silently: nobody reads session.csv
// until the drive is over, and by then a malformed row is unrecoverable —
// the run it was meant to measure has already happened. So the schema, the
// no-fix convention and the truncation contract are pinned here rather than
// discovered on a card afterwards.

#include <unity.h>

#include <string.h>

#include "../../src/analyzer_budget.h" // ANALYZER_STATIC_BYTES
#include "../../src/session_log.h"

// Counts comma-separated fields. Safe for this schema: every value is a
// number, a fixed keyword, or an ISO-8601 timestamp — none contain commas.
static int countFields(const char *s) {
    int n = 1;
    for (const char *p = s; *p; p++) {
        if (*p == ',') n++;
    }
    return n;
}

// A representative mid-drive sample: fix acquired, traffic flowing, nothing
// dropped.
static SessionStats healthySample() {
    SessionStats s;
    s.reason = "periodic";
    s.uptime_s = 3725;
    s.has_fix = true;
    s.lat = 37.774929;
    s.lon = -122.419418;
    s.sats = 14;
    s.sats_in_view = 21;
    s.fix_type = 3;
    s.ttff_s = 42;
    s.nmea_sentences = 58000;
    s.nmea_bad_crc = 3;
    s.rx = 912;
    s.crc_errors = 17;
    s.queue_drops = 0;
    s.bus_misses = 0;
    s.rows_written = 912;
    s.rows_dropped = 0;
    s.flushes = 71;
    s.max_flush_ms = 38;
    s.max_session_ms = 26;
    s.sd_ready = true;
    s.bus_contention = 0;
    s.heap_free = 338496;
    s.heap_min = 301112;
    s.batt_mv = 3765;
    s.logger_stack_free = 2144;
    s.run = 7;
    s.gps_max_loop_gap_ms = 18;
    s.gps_oversize_drops = 0;
    s.heap_largest = 200000;
    s.heap_free_blocks = 19;
    s.heap_allocated_blocks = 155;
    s.radio_stack_free = 3000;
    s.gps_stack_free = 2200;
    s.ui_stack_free = 2100;
    s.wifi_stack_free = 5000;
    s.scan_observations = 12;
    s.scan_observation_drops = 1;
    // Distinct nonzero values, same reasoning as the gps_max_loop_gap_ms/
    // gps_oversize_drops pair below: catches a swapped-argument bug.
    s.energy_observations = 6;
    s.energy_observation_drops = 4;
    s.probe_runs = 2;
    s.probe_cancels = 1;
    s.probe_timeouts = 8;
    s.probe_failures = 0;
    s.probe_recoveries = 2;
    s.probe_last_away_ms = 1900;
    s.identities_decoded = 11;
    s.identity_drops = 1;
    // Distinct nonzero values, same reasoning as the other counter groups
    // above — catches a swapped-argument bug.
    s.cell_observations = 9;
    s.cell_observation_drops = 2;
    s.cell_runs = 3;
    s.cell_cancels = 1;
    s.cell_failures = 0;
    s.cell_recoveries = 3;
    s.cell_last_away_ms = 1500;
    // The real compile-time constant, not an arbitrary test value — pins
    // this test to analyzer_state.h's actual structures, so a future change
    // to any of the four (WaterfallHistory/ScopeTrace/CaptureHistory/
    // NodeRoster) fails this test as a deliberate reminder to update the
    // documented memory-budget numbers (docs/STATUS.md/version.h), not a
    // silent drift.
    s.analyzer_static_bytes = ANALYZER_STATIC_BYTES;
    return s;
}

void test_header_column_count_matches_row() {
    // The header and the writer are separate format strings; only a test
    // stops them drifting apart, and a drifted health log is worse than
    // none because it looks authoritative.
    char row[320];
    size_t n = sessionFormatCsv(healthySample(), row, sizeof(row), "2026-08-23T04:15:00Z");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(countFields(SESSION_CSV_HEADER), countFields(row));
}

void test_row_with_fix_carries_position_and_counters() {
    char row[320];
    size_t n = sessionFormatCsv(healthySample(), row, sizeof(row), "2026-08-23T04:15:00Z");
    TEST_ASSERT_EQUAL_size_t(strlen(row), n);
    TEST_ASSERT_EQUAL_STRING(
        "2026-08-23T04:15:00Z,3725,periodic,37.774929,-122.419418,14,21,3,42,"
        "912,17,0,0,"
        "912,0,71,38,26,ok,0,"
        "58000,3,338496,301112,3765,2144,7,"
        "18,0,200000,19,155,3000,2200,2100,5000,12,1,6,4,2,1,8,0,2,1900,11,1,"
        "9,2,3,1,0,3,1500,6728",
        row);
}

void test_row_without_fix_leaves_coords_empty() {
    // Null Island is a real coordinate. A health row that claims it would
    // put the device in the Gulf of Guinea on any map that reads this file.
    SessionStats s = healthySample();
    s.has_fix = false;
    s.lat = 0.0;
    s.lon = 0.0;
    s.sats = 0;
    // In view but not yet used: the acquiring state, which is precisely what
    // the used-count alone cannot express.
    s.sats_in_view = 12;
    s.fix_type = 1;
    s.ttff_s = 0;

    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    TEST_ASSERT_TRUE(n > 0);
    // Empty timestamp, then uptime/reason, then two empty coordinate fields.
    const char *expectedPrefix = ",3725,periodic,,,0,12,1,0,";
    TEST_ASSERT_EQUAL_INT(0, strncmp(row, expectedPrefix, strlen(expectedPrefix)));
    // Specifically not 0,0 rendered as a number.
    TEST_ASSERT_NULL(strstr(row, "0.000000"));
    TEST_ASSERT_EQUAL_INT(countFields(SESSION_CSV_HEADER), countFields(row));
}

void test_acquiring_is_distinguishable_from_no_sky() {
    // The reason sats_in_view is logged at all. Both rows have no fix and
    // zero satellites used; only the in-view count says whether the antenna
    // can see sky, which is the difference between "wait" and "go outside".
    SessionStats acquiring = healthySample();
    acquiring.has_fix = false;
    acquiring.sats = 0;
    acquiring.fix_type = 1;
    acquiring.sats_in_view = 12;

    SessionStats noSky = acquiring;
    noSky.sats_in_view = 0;

    char a[320], b[320];
    TEST_ASSERT_TRUE(sessionFormatCsv(acquiring, a, sizeof(a), "") > 0);
    TEST_ASSERT_TRUE(sessionFormatCsv(noSky, b, sizeof(b), "") > 0);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
    TEST_ASSERT_NOT_NULL(strstr(a, ",0,12,1,"));
    TEST_ASSERT_NOT_NULL(strstr(b, ",0,0,1,"));
}

void test_boot_row_is_distinguishable() {
    // Boot rows are what make a mid-drive power cycle visible in a file
    // several sessions append to — without one, reset counters look like a
    // firmware fault rather than someone knocking the USB cable out.
    SessionStats s = healthySample();
    s.reason = "boot";
    s.uptime_s = 3;
    s.rx = 0;

    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",3,boot,"));
}

void test_sd_down_is_recorded_as_a_word_not_a_number() {
    SessionStats s = healthySample();
    s.sd_ready = false;
    char row[320];
    TEST_ASSERT_TRUE(sessionFormatCsv(s, row, sizeof(row), "") > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",down,"));
}

void test_heap_trough_is_reported_separately_from_the_sample() {
    // The whole point of heap_min: a 60s sample can walk straight past the
    // dip that actually ends a long run, so both numbers must survive into
    // the row as distinct fields.
    SessionStats s = healthySample();
    s.heap_free = 338496;
    s.heap_min = 91000;
    char row[320];
    TEST_ASSERT_TRUE(sessionFormatCsv(s, row, sizeof(row), "") > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",338496,91000,"));
}

void test_logger_stack_headroom_precedes_run() {
    // Reported every row so the answer to "was 5120 enough?" is visible
    // without replaying the run.
    SessionStats s = healthySample();
    s.logger_stack_free = 96;
    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",96,7"));
}

void test_gps_diagnostics_keep_their_append_only_positions() {
    // Added 2026-08-23, appended after `run` per this schema's own
    // append-only-at-the-end convention (rx_uptime_ms/logger_stack_free set
    // the precedent) so existing parsers keep working. Distinct nonzero
    // values in both positions catch a swapped-argument bug that equal or
    // zero values would hide.
    SessionStats s = healthySample();
    s.gps_max_loop_gap_ms = 143;
    s.gps_oversize_drops = 5;
    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",7,143,5,200000,"));
}

void test_phase7_memory_diagnostics_precede_probe_identity_and_cell_counters() {
    SessionStats s = healthySample();
    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    const char *suffix =
        "200000,19,155,3000,2200,2100,5000,12,1,6,4,2,1,8,0,2,1900,11,1,9,2,3,1,0,3,1500,6728";
    TEST_ASSERT_TRUE(n >= strlen(suffix));
    TEST_ASSERT_EQUAL_STRING(suffix, row + n - strlen(suffix));
}

void test_cell_diagnostics_precede_analyzer_static_bytes() {
    // Appended after identities_decoded/identity_drops, same append-only
    // convention gps_max_loop_gap_ms/gps_oversize_drops and
    // logger_stack_free established before it. No longer the row's own
    // last columns as of analyzer_static_bytes (Phase 10, 2026-09-04) —
    // see test_analyzer_static_bytes_is_the_last_column() below for that
    // claim now.
    SessionStats s = healthySample();
    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    const char *suffix = "9,2,3,1,0,3,1500,6728";
    TEST_ASSERT_TRUE(n >= strlen(suffix));
    TEST_ASSERT_EQUAL_STRING(suffix, row + n - strlen(suffix));
}

void test_analyzer_static_bytes_is_the_last_column() {
    // Phase 10's one memory number (docs/research/LoRaTrace-Phases-7-10-
    // Design.md §9), appended after cell_last_away_ms per this schema's own
    // append-only-at-the-end convention.
    SessionStats s = healthySample();
    s.analyzer_static_bytes = 12345;
    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), "");
    TEST_ASSERT_TRUE(n > 0);
    const char *suffix = "12345";
    TEST_ASSERT_TRUE(n >= strlen(suffix));
    TEST_ASSERT_EQUAL_STRING(suffix, row + n - strlen(suffix));
}

void test_truncation_is_reported() {
    // Same contract as detectionFormatCsv: 0 means "don't write this line".
    // A half-row on the card would corrupt every subsequent parse of the
    // file, which matters more here than losing one sample.
    char tiny[16];
    TEST_ASSERT_EQUAL_size_t(0, sessionFormatCsv(healthySample(), tiny, sizeof(tiny),
                                                 "2026-08-23T04:15:00Z"));
    TEST_ASSERT_EQUAL_size_t(0, sessionFormatCsv(healthySample(), nullptr, 320, ""));
    TEST_ASSERT_EQUAL_size_t(0, sessionFormatCsv(healthySample(), tiny, 0, ""));
}

void test_null_timestamp_is_tolerated() {
    // The logger passes whatever detectionFormatTimestamp() produced; a
    // crash in the health writer would take down the logger task and with
    // it the detection log — instrumentation must never be the thing that
    // ends the run it is measuring.
    char row[320];
    TEST_ASSERT_TRUE(sessionFormatCsv(healthySample(), row, sizeof(row), nullptr) > 0);
    TEST_ASSERT_EQUAL_INT(countFields(SESSION_CSV_HEADER), countFields(row));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_header_column_count_matches_row);
    RUN_TEST(test_row_with_fix_carries_position_and_counters);
    RUN_TEST(test_row_without_fix_leaves_coords_empty);
    RUN_TEST(test_acquiring_is_distinguishable_from_no_sky);
    RUN_TEST(test_boot_row_is_distinguishable);
    RUN_TEST(test_sd_down_is_recorded_as_a_word_not_a_number);
    RUN_TEST(test_heap_trough_is_reported_separately_from_the_sample);
    RUN_TEST(test_logger_stack_headroom_precedes_run);
    RUN_TEST(test_gps_diagnostics_keep_their_append_only_positions);
    RUN_TEST(test_phase7_memory_diagnostics_precede_probe_identity_and_cell_counters);
    RUN_TEST(test_cell_diagnostics_precede_analyzer_static_bytes);
    RUN_TEST(test_analyzer_static_bytes_is_the_last_column);
    RUN_TEST(test_truncation_is_reported);
    RUN_TEST(test_null_timestamp_is_tolerated);
    return UNITY_END();
}
