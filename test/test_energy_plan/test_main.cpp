#include <unity.h>

#include "../../src/energy_plan.h"

void test_energy_band_bounds_match_868_923() {
    TEST_ASSERT_EQUAL_FLOAT(868.0f, ENERGY_SWEEP_BAND_GLOBAL.lo_mhz);
    TEST_ASSERT_EQUAL_FLOAT(923.0f, ENERGY_SWEEP_BAND_GLOBAL.hi_mhz);
}

// 47 CFR § 15.247's US ISM floor (902MHz); high edge stays the module's
// own hardware ceiling, same as GLOBAL — see energy_plan.h's citation.
void test_energy_band_bounds_match_us_902_923() {
    TEST_ASSERT_EQUAL_FLOAT(902.0f, ENERGY_SWEEP_BAND_US.lo_mhz);
    TEST_ASSERT_EQUAL_FLOAT(923.0f, ENERGY_SWEEP_BAND_US.hi_mhz);
}

void test_energy_sweep_band_for_region_round_trips() {
    TEST_ASSERT_EQUAL_FLOAT(ENERGY_SWEEP_BAND_GLOBAL.lo_mhz,
                             energySweepBandForRegion(Region::GLOBAL).lo_mhz);
    TEST_ASSERT_EQUAL_FLOAT(ENERGY_SWEEP_BAND_US.lo_mhz,
                             energySweepBandForRegion(Region::US).lo_mhz);
}

void test_energy_bin_step_khz_matches_enum_value() {
    TEST_ASSERT_EQUAL_UINT16(250, energyBinStepKhz(EnergyBinStep::KHZ_250));
    TEST_ASSERT_EQUAL_UINT16(500, energyBinStepKhz(EnergyBinStep::KHZ_500));
}

void test_energy_bin_count_for_250khz_is_221_within_reserved_ceiling() {
    TEST_ASSERT_EQUAL_UINT16(221, energyBinCount(ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
    TEST_ASSERT_TRUE(energyBinCount(ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250) <=
                      ENERGY_BIN_RESERVED_COUNT);
}

void test_energy_bin_count_for_500khz_is_111() {
    TEST_ASSERT_EQUAL_UINT16(111, energyBinCount(ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_500));
}

// 902-923MHz at 250kHz: (923-902)/0.25 + 1 = 85 bins.
void test_energy_bin_count_for_us_250khz_is_85() {
    TEST_ASSERT_EQUAL_UINT16(85, energyBinCount(ENERGY_SWEEP_BAND_US, EnergyBinStep::KHZ_250));
}

void test_energy_bin_reserved_ceiling_is_224() {
    TEST_ASSERT_EQUAL_UINT16(224, ENERGY_BIN_RESERVED_COUNT);
}

void test_energy_bin_frequency_band_edges_250khz() {
    TEST_ASSERT_EQUAL_FLOAT(868.0f,
                             energyBinFrequencyMhz(0, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
    TEST_ASSERT_EQUAL_FLOAT(
        923.0f, energyBinFrequencyMhz(220, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
}

void test_energy_bin_frequency_band_edges_500khz() {
    TEST_ASSERT_EQUAL_FLOAT(868.0f,
                             energyBinFrequencyMhz(0, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_500));
    TEST_ASSERT_EQUAL_FLOAT(
        923.0f, energyBinFrequencyMhz(110, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_500));
}

void test_energy_bin_frequency_band_edges_us_250khz() {
    TEST_ASSERT_EQUAL_FLOAT(902.0f,
                             energyBinFrequencyMhz(0, ENERGY_SWEEP_BAND_US, EnergyBinStep::KHZ_250));
    TEST_ASSERT_EQUAL_FLOAT(923.0f,
                             energyBinFrequencyMhz(84, ENERGY_SWEEP_BAND_US, EnergyBinStep::KHZ_250));
}

void test_energy_bin_frequency_spacing_matches_step() {
    TEST_ASSERT_EQUAL_FLOAT(
        0.25f, energyBinFrequencyMhz(1, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250) -
                   energyBinFrequencyMhz(0, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
    TEST_ASSERT_EQUAL_FLOAT(
        0.5f, energyBinFrequencyMhz(1, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_500) -
                  energyBinFrequencyMhz(0, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_500));
}

void test_energy_bin_index_for_frequency_roundtrips_at_bin_centers() {
    const uint16_t indices[] = {0, 1, 110, 220};
    for (uint16_t i : indices) {
        const float freq = energyBinFrequencyMhz(i, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250);
        TEST_ASSERT_EQUAL_UINT16(
            i, energyBinIndexForFrequencyMhz(freq, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
    }
}

void test_energy_bin_index_for_frequency_clamps_out_of_band() {
    TEST_ASSERT_EQUAL_UINT16(
        0, energyBinIndexForFrequencyMhz(800.0f, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
    TEST_ASSERT_EQUAL_UINT16(
        220, energyBinIndexForFrequencyMhz(950.0f, ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250));
}

void test_energy_default_step_is_250khz() {
    TEST_ASSERT_TRUE(ENERGY_SWEEP_DEFAULT_STEP == EnergyBinStep::KHZ_250);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_energy_band_bounds_match_868_923);
    RUN_TEST(test_energy_band_bounds_match_us_902_923);
    RUN_TEST(test_energy_sweep_band_for_region_round_trips);
    RUN_TEST(test_energy_bin_step_khz_matches_enum_value);
    RUN_TEST(test_energy_bin_count_for_250khz_is_221_within_reserved_ceiling);
    RUN_TEST(test_energy_bin_count_for_500khz_is_111);
    RUN_TEST(test_energy_bin_count_for_us_250khz_is_85);
    RUN_TEST(test_energy_bin_reserved_ceiling_is_224);
    RUN_TEST(test_energy_bin_frequency_band_edges_250khz);
    RUN_TEST(test_energy_bin_frequency_band_edges_500khz);
    RUN_TEST(test_energy_bin_frequency_band_edges_us_250khz);
    RUN_TEST(test_energy_bin_frequency_spacing_matches_step);
    RUN_TEST(test_energy_bin_index_for_frequency_roundtrips_at_bin_centers);
    RUN_TEST(test_energy_bin_index_for_frequency_clamps_out_of_band);
    RUN_TEST(test_energy_default_step_is_250khz);
    return UNITY_END();
}
