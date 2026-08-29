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

void test_pass_b_confidence_matches_bench_data_only_for_the_two_reproduced_combos() {
    // Sourced from the 3-run, 1200-cycle shielded-box matrix
    // (research/phase9-sweep-pass-b-design.md's "Shielded-box quiet
    // control"): only SF8/BW125 and SF11/BW500 replicated across all three
    // independent setups. Everything else -- including every other real
    // sourced combo and Pass A's own sf=0/bw=0 sentinel -- stays UNVERIFIED.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PassBConfidence::STRONG,
                             (uint8_t)passBConfidenceFor(8, 1250));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PassBConfidence::NOISY,
                             (uint8_t)passBConfidenceFor(11, 5000));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PassBConfidence::UNVERIFIED,
                             (uint8_t)passBConfidenceFor(0, 0));
    for (uint8_t i = 0; i < PASS_B_SF_BW_CANDIDATE_COUNT; i++) {
        const PassBModemParams &c = PASS_B_SF_BW_CANDIDATES[i];
        const uint16_t bw_khz_x10 = (uint16_t)(c.bw_khz * 10.0f + 0.5f);
        const bool isHighCombo = (c.sf == 8 && bw_khz_x10 == 1250);
        const bool isNoisyCombo = (c.sf == 11 && bw_khz_x10 == 5000);
        if (!isHighCombo && !isNoisyCombo) {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)PassBConfidence::UNVERIFIED,
                                     (uint8_t)passBConfidenceFor(c.sf, bw_khz_x10));
        }
    }
}

void test_pass_b_confidence_name_round_trips() {
    TEST_ASSERT_EQUAL_STRING("high", passBConfidenceName(PassBConfidence::STRONG));
    TEST_ASSERT_EQUAL_STRING("noisy", passBConfidenceName(PassBConfidence::NOISY));
    TEST_ASSERT_EQUAL_STRING("unverified", passBConfidenceName(PassBConfidence::UNVERIFIED));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pass_b_table_is_small_bounded_and_in_band);
    RUN_TEST(test_pass_b_table_has_no_duplicate_sf_bw_pairs);
    RUN_TEST(test_pass_b_max_peaks_matches_design_doc_starting_bound);
    RUN_TEST(test_pass_b_confidence_matches_bench_data_only_for_the_two_reproduced_combos);
    RUN_TEST(test_pass_b_confidence_name_round_trips);
    return UNITY_END();
}
