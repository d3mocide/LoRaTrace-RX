#include <unity.h>

#include <string.h>

#include "../../src/energy_observation.h"
#include "../../src/energy_plan.h"

void test_energy_bin_stats_is_compact_and_fits_reserved_table_budget() {
    TEST_ASSERT_TRUE(sizeof(EnergyBinStats) <= 8);
    TEST_ASSERT_TRUE((size_t)ENERGY_BIN_RESERVED_COUNT * sizeof(EnergyBinStats) <= 1800);
}

void test_energy_observation_is_compact_and_separate_from_detection_and_scan_observation() {
    TEST_ASSERT_TRUE(sizeof(EnergyObservation) <= 32);
    TEST_ASSERT_EQUAL_STRING(
        "energy_peak", energyObservationResultName(EnergyObservationResult::ENERGY_PEAK));
}

// Phase 9 Pass B result names, appended (not inserted) after ENERGY_PEAK/
// RADIO_ERROR so any already-written energy.csv keeps its existing values
// (research/phase9-sweep-pass-b-design.md).
void test_pass_b_result_names() {
    TEST_ASSERT_EQUAL_STRING("cad_free", energyObservationResultName(EnergyObservationResult::CAD_FREE));
    TEST_ASSERT_EQUAL_STRING("cad_detected", energyObservationResultName(EnergyObservationResult::CAD_DETECTED));
    TEST_ASSERT_EQUAL_STRING("cad_timeout", energyObservationResultName(EnergyObservationResult::CAD_TIMEOUT));
}

void test_energy_bin_stats_add_sample_running_average_and_peak() {
    EnergyBinStats stats;
    energyBinStatsAddSample(stats, -900);
    energyBinStatsAddSample(stats, -800);
    energyBinStatsAddSample(stats, -700);
    TEST_ASSERT_EQUAL_INT16(-800, stats.rssi_avg_dbm_x10);
    TEST_ASSERT_EQUAL_INT16(-700, stats.rssi_peak_dbm_x10);
    TEST_ASSERT_EQUAL_UINT8(3, stats.sample_count);
}

void test_energy_bin_stats_add_sample_overflow_flag() {
    EnergyBinStats stats;
    stats.sample_count = 255;
    energyBinStatsAddSample(stats, -900);
    TEST_ASSERT_EQUAL_UINT8(255, stats.sample_count);
    TEST_ASSERT_TRUE(stats.flags & ENERGY_BIN_FLAG_SAMPLE_OVERFLOW);
}

void test_energy_bin_stats_note_occupancy_counts_only_when_occupied() {
    EnergyBinStats stats;
    energyBinStatsNoteOccupancy(stats, true);
    energyBinStatsNoteOccupancy(stats, true);
    energyBinStatsNoteOccupancy(stats, false);
    energyBinStatsNoteOccupancy(stats, true);
    TEST_ASSERT_EQUAL_UINT8(3, stats.occupied_count);
}

void test_energy_noise_floor_update_tracks_toward_sample() {
    TEST_ASSERT_EQUAL_INT16(-1000, energyNoiseFloorUpdate(-1000, -1000));
    TEST_ASSERT_EQUAL_INT16(-900, energyNoiseFloorUpdate(-1000, -200));
    TEST_ASSERT_EQUAL_INT16(-1100, energyNoiseFloorUpdate(-1000, -1800));
}

void test_energy_noise_floor_update_truncates_small_deltas_toward_zero() {
    // delta=5, 5/8 truncates to 0 under integer division: deliberate slow
    // convergence, not a bug.
    TEST_ASSERT_EQUAL_INT16(-1000, energyNoiseFloorUpdate(-1000, -995));
}

void test_energy_exceeds_floor_boundary_cases() {
    TEST_ASSERT_FALSE(energyExceedsFloor(-1200, -1000, 100));
    TEST_ASSERT_FALSE(energyExceedsFloor(-901, -1000, 100));
    TEST_ASSERT_TRUE(energyExceedsFloor(-900, -1000, 100));
    TEST_ASSERT_TRUE(energyExceedsFloor(-700, -1000, 100));
}

void test_energy_bin_is_peak_uses_peak_not_average() {
    EnergyBinStats stats;
    energyBinStatsAddSample(stats, -1200);
    energyBinStatsAddSample(stats, -1200);
    energyBinStatsAddSample(stats, -500);
    // avg ~ -967, which would not clear a -900 threshold; peak (-500) does.
    TEST_ASSERT_TRUE(energyBinIsPeak(stats, -1000, 100));
}

void test_energy_bin_is_peak_false_when_no_samples() {
    EnergyBinStats stats;
    TEST_ASSERT_FALSE(energyBinIsPeak(stats, -1000, 100));
}

void test_energy_rssi_dbm_to_fixed_rounds_and_clamps() {
    TEST_ASSERT_EQUAL_INT16(-655, energyRssiDbmToFixed(-65.5f));
    TEST_ASSERT_EQUAL_INT16(-1000, energyRssiDbmToFixed(-100.04f));
    TEST_ASSERT_EQUAL_INT16(-32768, energyRssiDbmToFixed(-5000.0f));
}

void test_energy_observation_csv_with_fix() {
    EnergyObservation observation;
    observation.rx_millis = 555000;
    observation.freq_mhz = 878.5f;
    observation.bw_khz_x10 = 0;
    observation.bin_step_khz = 250;
    observation.rssi_avg_dbm_x10 = -650;
    observation.rssi_peak_dbm_x10 = -580;
    observation.radio_status = 0;
    observation.profile = (uint8_t)MissionProfile::GENERAL_EXPLORATION;
    observation.bin_index = 42;
    observation.sf = 0;
    observation.cr_denom = 0;
    observation.sync_word = 0;
    observation.sample_count = 8;
    observation.result = EnergyObservationResult::ENERGY_PEAK;
    observation.packet_metadata_present = false;
    observation.wifi_on = false;

    char row[256];
    const size_t n = energyObservationFormatCsv(observation, row, sizeof(row),
                                                "2026-08-28T10:00:00Z", true,
                                                45.5, -122.6, 1, 3);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "2026-08-28T10:00:00Z,45.500000,-122.600000,1,3,555000,general,energy_peak,"
        "42,250,878.500,0,0.0,0,0x00,-65.0,-58.0,8,0,0,0,unverified",
        row);
}

// research/phase9-sweep-pass-b-design.md's "Shielded-box quiet control":
// only these two combos have earned a non-default confidence tag so far.
void test_energy_observation_csv_pass_b_confidence_for_high_combo() {
    EnergyObservation observation;
    observation.sf = 8;
    observation.bw_khz_x10 = 1250;
    observation.result = EnergyObservationResult::CAD_DETECTED;

    char row[256];
    TEST_ASSERT_TRUE(energyObservationFormatCsv(observation, row, sizeof(row), "", false,
                                                0.0, 0.0, 0, 1) > 0);
    TEST_ASSERT_TRUE(strstr(row, ",high") != nullptr);
}

void test_energy_observation_csv_pass_b_confidence_for_noisy_combo() {
    EnergyObservation observation;
    observation.sf = 11;
    observation.bw_khz_x10 = 5000;
    observation.result = EnergyObservationResult::CAD_DETECTED;

    char row[256];
    TEST_ASSERT_TRUE(energyObservationFormatCsv(observation, row, sizeof(row), "", false,
                                                0.0, 0.0, 0, 1) > 0);
    TEST_ASSERT_TRUE(strstr(row, ",noisy") != nullptr);
}

void test_energy_observation_csv_without_fix_is_honest() {
    EnergyObservation observation;
    observation.rx_millis = 777;
    observation.profile = (uint8_t)MissionProfile::RETICULUM;
    observation.result = EnergyObservationResult::RADIO_ERROR;
    observation.radio_status = -2;

    char row[256];
    TEST_ASSERT_TRUE(energyObservationFormatCsv(observation, row, sizeof(row), "", false,
                                                0.0, 0.0, 0, 1) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",,,0,1,777,reticulum,radio_error,"));
    TEST_ASSERT_NULL(strstr(row, "0.000000"));
}

void test_energy_observation_csv_truncation_is_reported() {
    EnergyObservation observation;
    char row[8];
    TEST_ASSERT_EQUAL_size_t(0, energyObservationFormatCsv(observation, row, sizeof(row),
                                                            "timestamp", false, 0.0, 0.0, 0, 1));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_energy_bin_stats_is_compact_and_fits_reserved_table_budget);
    RUN_TEST(test_energy_observation_is_compact_and_separate_from_detection_and_scan_observation);
    RUN_TEST(test_pass_b_result_names);
    RUN_TEST(test_energy_bin_stats_add_sample_running_average_and_peak);
    RUN_TEST(test_energy_bin_stats_add_sample_overflow_flag);
    RUN_TEST(test_energy_bin_stats_note_occupancy_counts_only_when_occupied);
    RUN_TEST(test_energy_noise_floor_update_tracks_toward_sample);
    RUN_TEST(test_energy_noise_floor_update_truncates_small_deltas_toward_zero);
    RUN_TEST(test_energy_exceeds_floor_boundary_cases);
    RUN_TEST(test_energy_bin_is_peak_uses_peak_not_average);
    RUN_TEST(test_energy_bin_is_peak_false_when_no_samples);
    RUN_TEST(test_energy_rssi_dbm_to_fixed_rounds_and_clamps);
    RUN_TEST(test_energy_observation_csv_with_fix);
    RUN_TEST(test_energy_observation_csv_without_fix_is_honest);
    RUN_TEST(test_energy_observation_csv_truncation_is_reported);
    RUN_TEST(test_energy_observation_csv_pass_b_confidence_for_high_combo);
    RUN_TEST(test_energy_observation_csv_pass_b_confidence_for_noisy_combo);
    return UNITY_END();
}
