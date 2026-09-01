#include <unity.h>

#include "../../src/cell_plan.h"

void test_cell_band_bounds_match_869_894() {
    TEST_ASSERT_EQUAL_FLOAT(869.0f, CELL_SWEEP_BAND_LO_MHZ);
    TEST_ASSERT_EQUAL_FLOAT(894.0f, CELL_SWEEP_BAND_HI_MHZ);
}

void test_cell_bin_count_is_101_within_reserved_ceiling() {
    TEST_ASSERT_EQUAL_UINT16(101, cellBinCount());
    TEST_ASSERT_TRUE(cellBinCount() <= CELL_BIN_RESERVED_COUNT);
}

void test_cell_bin_frequency_band_edges() {
    TEST_ASSERT_EQUAL_FLOAT(869.0f, cellBinFrequencyMhz(0));
    TEST_ASSERT_EQUAL_FLOAT(894.0f, cellBinFrequencyMhz(100));
}

void test_cell_bin_frequency_spacing_matches_step() {
    TEST_ASSERT_EQUAL_FLOAT(0.25f, cellBinFrequencyMhz(1) - cellBinFrequencyMhz(0));
}

void test_cell_bin_index_for_frequency_roundtrips_at_bin_centers() {
    const uint16_t indices[] = {0, 1, 50, 100};
    for (uint16_t i : indices) {
        const float freq = cellBinFrequencyMhz(i);
        TEST_ASSERT_EQUAL_UINT16(i, cellBinIndexForFrequencyMhz(freq));
    }
}

void test_cell_bin_index_for_frequency_clamps_out_of_band() {
    TEST_ASSERT_EQUAL_UINT16(0, cellBinIndexForFrequencyMhz(800.0f));
    TEST_ASSERT_EQUAL_UINT16(100, cellBinIndexForFrequencyMhz(950.0f));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_cell_band_bounds_match_869_894);
    RUN_TEST(test_cell_bin_count_is_101_within_reserved_ceiling);
    RUN_TEST(test_cell_bin_frequency_band_edges);
    RUN_TEST(test_cell_bin_frequency_spacing_matches_step);
    RUN_TEST(test_cell_bin_index_for_frequency_roundtrips_at_bin_centers);
    RUN_TEST(test_cell_bin_index_for_frequency_clamps_out_of_band);
    return UNITY_END();
}
