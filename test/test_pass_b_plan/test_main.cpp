#include <unity.h>

#include "../../src/pass_b_plan.h"

void test_pass_b_table_is_small_bounded_and_in_band() {
    // "Small sourced set", not the unbounded product the design doc warns
    // against — 10 real, already-established combos.
    TEST_ASSERT_EQUAL_UINT8(10, PASS_B_SF_BW_CANDIDATE_COUNT);
    for (uint8_t i = 0; i < PASS_B_SF_BW_CANDIDATE_COUNT; i++) {
        const PassBModemParams &c = PASS_B_SF_BW_CANDIDATES[i];
        TEST_ASSERT_TRUE(c.sf >= 7 && c.sf <= 12);
        TEST_ASSERT_TRUE(c.bw_khz > 0.0f);
        TEST_ASSERT_EQUAL_UINT8(PASS_B_CR_DENOM_PLACEHOLDER, c.cr_denom);
        TEST_ASSERT_EQUAL_UINT8(PASS_B_SYNC_WORD_PLACEHOLDER, c.sync_word);
    }
}

void test_pass_b_table_has_no_duplicate_sf_bw_pairs() {
    for (uint8_t i = 0; i < PASS_B_SF_BW_CANDIDATE_COUNT; i++) {
        for (uint8_t j = 0; j < i; j++) {
            const bool same = PASS_B_SF_BW_CANDIDATES[i].sf == PASS_B_SF_BW_CANDIDATES[j].sf &&
                               PASS_B_SF_BW_CANDIDATES[i].bw_khz == PASS_B_SF_BW_CANDIDATES[j].bw_khz;
            TEST_ASSERT_FALSE(same);
        }
    }
}

void test_pass_b_max_peaks_matches_design_doc_starting_bound() {
    TEST_ASSERT_EQUAL_UINT16(8, PASS_B_MAX_PEAKS_PER_SWEEP);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pass_b_table_is_small_bounded_and_in_band);
    RUN_TEST(test_pass_b_table_has_no_duplicate_sf_bw_pairs);
    RUN_TEST(test_pass_b_max_peaks_matches_design_doc_starting_bound);
    return UNITY_END();
}
