#include <unity.h>

#include <string.h>

#include "../../src/focus_observation.h"

void test_focus_working_state_stays_below_one_result_budget() {
    TEST_ASSERT_EQUAL_size_t(148, sizeof(FocusRssiHistogram));
    TEST_ASSERT_EQUAL_size_t(40, sizeof(FocusObservation));
    TEST_ASSERT_EQUAL_size_t(188, sizeof(FocusRssiHistogram) + sizeof(FocusObservation));
}

void test_focus_histogram_reports_median_p90_and_peak_without_raw_samples() {
    FocusRssiHistogram histogram;
    focusHistogramAddSample(histogram, -1000);
    focusHistogramAddSample(histogram, -900);
    focusHistogramAddSample(histogram, -800);
    focusHistogramAddSample(histogram, -700);
    focusHistogramAddSample(histogram, -600);

    TEST_ASSERT_EQUAL_UINT16(5, histogram.sample_count);
    TEST_ASSERT_EQUAL_INT16(-800, focusHistogramMedianDbmX10(histogram));
    TEST_ASSERT_EQUAL_INT16(-600, focusHistogramP90DbmX10(histogram));
    TEST_ASSERT_EQUAL_INT16(-600, histogram.peak_dbm_x10);
}

void test_focus_histogram_rounds_to_one_db_and_flags_clamped_input() {
    FocusRssiHistogram histogram;
    focusHistogramAddSample(histogram, -1005); // nearest 1dB bin: -101dBm
    focusHistogramAddSample(histogram, -1600); // below documented range
    focusHistogramAddSample(histogram, 50);    // above documented range

    TEST_ASSERT_EQUAL_INT16(-1010, focusHistogramMedianDbmX10(histogram));
    TEST_ASSERT_TRUE(histogram.flags & FOCUS_RSSI_FLAG_UNDERFLOW);
    TEST_ASSERT_TRUE(histogram.flags & FOCUS_RSSI_FLAG_OVERFLOW);
}

void test_focus_histogram_refuses_quantiles_after_bucket_saturation() {
    FocusRssiHistogram histogram;
    for (uint16_t i = 0; i < 256; ++i) focusHistogramAddSample(histogram, -800);
    TEST_ASSERT_TRUE(histogram.flags & FOCUS_RSSI_FLAG_BUCKET_OVERFLOW);
    TEST_ASSERT_EQUAL_INT16(FOCUS_RSSI_NO_SAMPLE_DBM_X10,
                            focusHistogramMedianDbmX10(histogram));
}

void test_focus_request_status_names_are_explicit() {
    TEST_ASSERT_EQUAL_STRING("complete", focusRequestStatusName(FocusRequestStatus::COMPLETE));
    TEST_ASSERT_EQUAL_STRING("cancelled", focusRequestStatusName(FocusRequestStatus::CANCELLED));
    TEST_ASSERT_EQUAL_STRING("timeout", focusRequestStatusName(FocusRequestStatus::TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("failed", focusRequestStatusName(FocusRequestStatus::FAILED));
}

void test_focus_csv_with_fix_persists_blank_coverage_and_raw_counts() {
    FocusObservation observation;
    observation.rx_millis = 555000;
    observation.observation_ms = 470;
    observation.freq_mhz = 912.75f;
    observation.focus_id = 7;
    observation.selection_bin_index = 43;
    observation.requested_dwell_ms = 500;
    observation.requested_samples = 4;
    observation.sample_count = 4;
    observation.qualifying_count = 0;
    observation.rssi_median_dbm_x10 = -920;
    observation.rssi_p90_dbm_x10 = -850;
    observation.rssi_peak_dbm_x10 = -850;
    observation.profile = (uint8_t)MissionProfile::GENERAL_EXPLORATION;
    observation.requested_passes = 1;
    observation.valid_passes = 1;
    observation.selection_source = FocusSelectionSource::SWEEP_BIN;
    observation.request_status = FocusRequestStatus::COMPLETE;
    observation.home_restore = true;

    char row[256];
    const size_t n = focusObservationFormatCsv(observation, row, sizeof(row),
                                               "2026-09-04T10:00:00Z", true,
                                               45.5, -122.6, 1, 3);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "2026-09-04T10:00:00Z,45.500000,-122.600000,1,3,555000,general,7,sweep,43,"
        "912.750,1,1,500,470,4,4,-92.0,-85.0,-85.0,0,,complete,1,0,0", row);
}

void test_focus_csv_without_fix_or_samples_keeps_unknown_values_blank() {
    FocusObservation observation;
    observation.rx_millis = 777;
    observation.profile = (uint8_t)MissionProfile::RETICULUM;
    observation.request_status = FocusRequestStatus::FAILED;
    observation.radio_status = -2;

    char row[256];
    TEST_ASSERT_TRUE(focusObservationFormatCsv(observation, row, sizeof(row), "", false,
                                               0.0, 0.0, 0, 1) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",,,0,1,777,reticulum,"));
    TEST_ASSERT_NULL(strstr(row, "0.000000"));
    TEST_ASSERT_NOT_NULL(strstr(row, ",0,0,,,,0,,failed,"));
}

void test_focus_csv_truncation_is_reported() {
    FocusObservation observation;
    char row[8];
    TEST_ASSERT_EQUAL_size_t(0, focusObservationFormatCsv(observation, row, sizeof(row),
                                                           "timestamp", false,
                                                           0.0, 0.0, 0, 1));
}

void test_worst_case_row_fits_the_logger_buffer_with_margin() {
    // Every field at its widest, including widths the bounded request cannot
    // actually produce. An over-long row is dropped rather than truncated, so
    // this must hold at the maximum, not at today's typical bench values.
    FocusObservation observation;
    observation.rx_millis = 4294967295u;
    observation.observation_ms = 4294967295u;
    observation.freq_mhz = 928.1234f;
    observation.focus_id = 65535;
    observation.selection_bin_index = 65535;
    observation.requested_dwell_ms = 65535;
    observation.requested_samples = 65535;
    observation.sample_count = 65535;
    observation.qualifying_count = 65535;
    observation.rssi_median_dbm_x10 = -1405;
    observation.rssi_p90_dbm_x10 = -1405;
    observation.rssi_peak_dbm_x10 = -1405;
    observation.radio_status = -32768;
    observation.profile = 200;  // longest name path: "unknown"
    observation.requested_passes = 255;
    observation.valid_passes = 255;
    observation.selection_source = FocusSelectionSource::WATERFALL_BIN;  // "waterfall"
    observation.request_status = FocusRequestStatus::CANCELLED;          // "cancelled"
    observation.home_restore = true;
    observation.wifi_on = true;

    char row[FOCUS_CSV_ROW_MAX];
    const size_t n = focusObservationFormatCsv(observation, row, sizeof(row),
                                               "2026-09-04T21:57:36Z", true,
                                               -179.987654, -179.987654, 255, 65535);
    TEST_ASSERT_TRUE(n > 0);
    // The logger appends a newline at row[n], so n + 1 must still fit.
    TEST_ASSERT_TRUE(n + 1 <= FOCUS_CSV_ROW_MAX);
    // Measured at 189 bytes; fail loudly if a schema change eats the margin.
    TEST_ASSERT_TRUE(n <= 200);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_focus_working_state_stays_below_one_result_budget);
    RUN_TEST(test_focus_histogram_reports_median_p90_and_peak_without_raw_samples);
    RUN_TEST(test_focus_histogram_rounds_to_one_db_and_flags_clamped_input);
    RUN_TEST(test_focus_histogram_refuses_quantiles_after_bucket_saturation);
    RUN_TEST(test_focus_request_status_names_are_explicit);
    RUN_TEST(test_focus_csv_with_fix_persists_blank_coverage_and_raw_counts);
    RUN_TEST(test_focus_csv_without_fix_or_samples_keeps_unknown_values_blank);
    RUN_TEST(test_focus_csv_truncation_is_reported);
    RUN_TEST(test_worst_case_row_fits_the_logger_buffer_with_margin);
    return UNITY_END();
}
