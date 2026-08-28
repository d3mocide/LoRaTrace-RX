#include <unity.h>

#include <string.h>

#include "../../src/scan_observation.h"

void test_scan_observation_is_small_and_separate_from_detection() {
    TEST_ASSERT_TRUE(sizeof(ScanObservation) <= 24);
    TEST_ASSERT_EQUAL_STRING("cad_detected",
                             scanObservationResultName(ScanObservationResult::CAD_DETECTED));
}

void test_scan_observation_csv_with_fix() {
    ScanObservation observation;
    observation.rx_millis = 123456;
    observation.freq_mhz = 915.0f;
    observation.radio_status = 0;
    observation.bw_khz_x10 = 2500;
    observation.sf = 10;
    observation.cr_denom = 5;
    observation.sync_word = 0x12;
    observation.profile = (uint8_t)MissionProfile::MESHCORE;
    observation.candidate_index = 1;
    observation.result = ScanObservationResult::CAD_DETECTED;

    char row[256];
    const size_t n = scanObservationFormatCsv(observation, row, sizeof(row),
                                              "2026-08-27T12:00:00Z", true,
                                              37.5, -122.4, 1, 12);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "2026-08-27T12:00:00Z,37.500000,-122.400000,1,12,123456,meshcore,"
        "cad_detected,915.000,10,250.0,5,0x12,1,0",
        row);
}

void test_scan_observation_csv_without_fix_is_honest() {
    ScanObservation observation;
    observation.profile = (uint8_t)MissionProfile::MESHTASTIC;
    observation.result = ScanObservationResult::CAD_TIMEOUT;

    char row[256];
    TEST_ASSERT_TRUE(scanObservationFormatCsv(observation, row, sizeof(row),
                                              "", false, 0.0, 0.0, 0, 1) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",,,0,1,0,meshtastic,cad_timeout,"));
    TEST_ASSERT_NULL(strstr(row, "0.000000"));
}

void test_scan_observation_csv_truncation_is_reported() {
    ScanObservation observation;
    char row[8];
    TEST_ASSERT_EQUAL_size_t(0, scanObservationFormatCsv(
                                   observation, row, sizeof(row), "timestamp",
                                   false, 0.0, 0.0, 0, 1));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_scan_observation_is_small_and_separate_from_detection);
    RUN_TEST(test_scan_observation_csv_with_fix);
    RUN_TEST(test_scan_observation_csv_without_fix_is_honest);
    RUN_TEST(test_scan_observation_csv_truncation_is_reported);
    return UNITY_END();
}
