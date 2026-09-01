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

// FCC A/B blocks (47 CFR S 22.905) must tile the whole 869-894MHz band
// with no gaps or overlaps: first block starts at the band floor, last
// block ends at the band ceiling, and each block's high edge is the next
// block's low edge.
void test_cell_band_blocks_tile_869_894_with_no_gaps() {
    TEST_ASSERT_EQUAL_FLOAT(CELL_SWEEP_BAND_LO_MHZ, CELL_BAND_BLOCKS[0].lo_mhz);
    TEST_ASSERT_EQUAL_FLOAT(CELL_SWEEP_BAND_HI_MHZ,
                             CELL_BAND_BLOCKS[CELL_BAND_BLOCK_COUNT - 1].hi_mhz);
    for (uint8_t i = 0; i + 1 < CELL_BAND_BLOCK_COUNT; i++) {
        TEST_ASSERT_EQUAL_FLOAT(CELL_BAND_BLOCKS[i].hi_mhz, CELL_BAND_BLOCKS[i + 1].lo_mhz);
    }
}

// Pins the citation's actual values (47 CFR S 22.905): A, B, A, B in band
// order, not a re-derivation.
void test_cell_band_blocks_alternate_a_b() {
    TEST_ASSERT_EQUAL_UINT16(4, CELL_BAND_BLOCK_COUNT);
    TEST_ASSERT_EQUAL_INT('A', CELL_BAND_BLOCKS[0].label);
    TEST_ASSERT_EQUAL_INT('B', CELL_BAND_BLOCKS[1].label);
    TEST_ASSERT_EQUAL_INT('A', CELL_BAND_BLOCKS[2].label);
    TEST_ASSERT_EQUAL_INT('B', CELL_BAND_BLOCKS[3].label);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_cell_band_bounds_match_869_894);
    RUN_TEST(test_cell_bin_count_is_101_within_reserved_ceiling);
    RUN_TEST(test_cell_bin_frequency_band_edges);
    RUN_TEST(test_cell_bin_frequency_spacing_matches_step);
    RUN_TEST(test_cell_bin_index_for_frequency_roundtrips_at_bin_centers);
    RUN_TEST(test_cell_bin_index_for_frequency_clamps_out_of_band);
    RUN_TEST(test_cell_band_blocks_tile_869_894_with_no_gaps);
    RUN_TEST(test_cell_band_blocks_alternate_a_b);
    return UNITY_END();
}
