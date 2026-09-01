#include <unity.h>

#include <string.h>

#include "../../src/cell_observation.h"

void test_cell_observation_is_small_and_separate_from_energy() {
    TEST_ASSERT_TRUE(sizeof(CellObservation) <= 24);
    TEST_ASSERT_EQUAL_STRING("measured", cellObservationResultName(CellObservationResult::MEASURED));
    TEST_ASSERT_EQUAL_STRING("radio_error",
                             cellObservationResultName(CellObservationResult::RADIO_ERROR));
}

void test_cell_observation_csv_with_fix() {
    CellObservation observation;
    observation.rx_millis = 123456;
    observation.freq_mhz = 880.0f;
    observation.rx_bw_khz_x10 = 1250;
    observation.rssi_avg_dbm_x10 = -650;
    observation.rssi_peak_dbm_x10 = -600;
    observation.radio_status = 0;
    observation.profile = (uint8_t)MissionProfile::MESHTASTIC;
    observation.bin_index = 44;
    observation.sample_count = 4;
    observation.result = CellObservationResult::MEASURED;

    char row[256];
    const size_t n = cellObservationFormatCsv(observation, row, sizeof(row),
                                              "2026-08-27T12:00:00Z", true,
                                              37.5, -122.4, 1, 12);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "2026-08-27T12:00:00Z,37.500000,-122.400000,1,12,123456,meshtastic,"
        "measured,44,880.000,125.0,-65.0,-60.0,4,0",
        row);
}

void test_cell_observation_csv_without_fix_is_honest() {
    CellObservation observation;
    observation.profile = (uint8_t)MissionProfile::MESHCORE;
    observation.result = CellObservationResult::RADIO_ERROR;

    char row[256];
    TEST_ASSERT_TRUE(cellObservationFormatCsv(observation, row, sizeof(row), "", false, 0.0, 0.0, 0,
                                              1) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",,,0,1,0,meshcore,radio_error,"));
    TEST_ASSERT_NULL(strstr(row, "0.000000"));
}

void test_cell_observation_csv_truncation_is_reported() {
    CellObservation observation;
    char row[8];
    TEST_ASSERT_EQUAL_size_t(
        0, cellObservationFormatCsv(observation, row, sizeof(row), "timestamp", false, 0.0, 0.0, 0, 1));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_cell_observation_is_small_and_separate_from_energy);
    RUN_TEST(test_cell_observation_csv_with_fix);
    RUN_TEST(test_cell_observation_csv_without_fix_is_honest);
    RUN_TEST(test_cell_observation_csv_truncation_is_reported);
    return UNITY_END();
}
