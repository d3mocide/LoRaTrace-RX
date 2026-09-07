// Workstream 12 §3/§3.1 — coverage labels and the accumulator behind them.
//
// Coverage says how much looking a bin has had, and nothing else. These tests
// are mostly about what it must REFUSE to say: a label from an invalid pass, a
// label carried across a retune, or a label earned by passes too short to mean
// anything.

#include <unity.h>

#include "focus_coverage.h"

namespace {

constexpr uint16_t BIN = 43;
constexpr uint16_t SHIPPED_DWELL_MS = 2000;

FocusCoverage fresh() { return FocusCoverage{}; }

void notePasses(FocusCoverage &c, int count, uint16_t bin = BIN,
                uint16_t dwell = SHIPPED_DWELL_MS, uint32_t observed = SHIPPED_DWELL_MS,
                bool valid = true) {
    for (int i = 0; i < count; i++) focusCoverageNote(c, bin, dwell, observed, valid);
}

} // namespace

void setUp() {}
void tearDown() {}

void test_nothing_observed_is_insufficient() {
    FocusCoverage c = fresh();
    TEST_ASSERT_EQUAL(FocusCoverageLabel::INSUFFICIENT, focusCoverageLabelFor(c));
    TEST_ASSERT_EQUAL_STRING("insufficient",
                             focusCoverageLabelName(focusCoverageLabelFor(c)));
}

void test_one_shipped_pass_is_sampled() {
    // Focus IS one deliberate look, so a single 2,000 ms pass must earn the
    // label that says the device may describe what it saw.
    FocusCoverage c = fresh();
    notePasses(c, 1);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::SAMPLED, focusCoverageLabelFor(c));
}

void test_three_shipped_passes_are_repeated() {
    FocusCoverage c = fresh();
    notePasses(c, 2);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::SAMPLED, focusCoverageLabelFor(c));
    notePasses(c, 1);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::REPEATED, focusCoverageLabelFor(c));
    TEST_ASSERT_EQUAL_STRING("repeated", focusCoverageLabelName(focusCoverageLabelFor(c)));
}

void test_both_halves_of_a_pair_must_be_met() {
    // Pass count alone must not earn a label. Three passes that each observed
    // only 1,000 ms accumulate 3,000 ms, short of REPEATED's 6,000 — so this
    // stays sampled. Guards the case a variable dwell would create, where the
    // pass count and the time no longer imply each other.
    FocusCoverage c = fresh();
    notePasses(c, 3, BIN, 1000, 1000);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::SAMPLED, focusCoverageLabelFor(c));
    TEST_ASSERT_EQUAL_UINT16(3, c.valid_passes);
    TEST_ASSERT_EQUAL_UINT32(3000, c.observation_ms);
}

void test_short_passes_never_count() {
    // §6.4: a pass under the dwell floor cannot both catch a source and reject
    // ambient at any threshold, and the coverage campaign found it also costs
    // more Watch time per second observed. Repeating it must not launder it
    // into a label.
    FocusCoverage c = fresh();
    notePasses(c, 20, BIN, 250, 250);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::INSUFFICIENT, focusCoverageLabelFor(c));
    TEST_ASSERT_EQUAL_UINT16(0, c.valid_passes);
    TEST_ASSERT_EQUAL_UINT32(0, c.observation_ms);
}

void test_invalid_passes_never_count() {
    // §3: a valid pass is not "a quiet channel" — it is one that configured the
    // frequency, produced samples, and was neither cancelled nor error
    // terminated. A cancelled request must not accumulate coverage.
    FocusCoverage c = fresh();
    notePasses(c, 5, BIN, SHIPPED_DWELL_MS, SHIPPED_DWELL_MS, /*valid=*/false);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::INSUFFICIENT, focusCoverageLabelFor(c));
    TEST_ASSERT_EQUAL_UINT16(0, c.valid_passes);
}

void test_changing_bin_resets_accumulated_coverage() {
    // The important one. Accumulated observation time means time on THAT
    // frequency; carrying it across a retune would claim coverage of a bin
    // nobody looked at.
    FocusCoverage c = fresh();
    notePasses(c, 3);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::REPEATED, focusCoverageLabelFor(c));

    focusCoverageNote(c, BIN + 1, SHIPPED_DWELL_MS, SHIPPED_DWELL_MS, true);
    TEST_ASSERT_EQUAL_UINT16(BIN + 1, c.bin_index);
    TEST_ASSERT_EQUAL_UINT16(1, c.valid_passes);
    TEST_ASSERT_EQUAL_UINT32(SHIPPED_DWELL_MS, c.observation_ms);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::SAMPLED, focusCoverageLabelFor(c));
}

void test_returning_to_a_bin_does_not_restore_its_old_coverage() {
    // Corollary of the reset: coverage is not remembered per bin, and coming
    // back must start over rather than resurrect a stale total.
    FocusCoverage c = fresh();
    notePasses(c, 3);
    focusCoverageNote(c, BIN + 1, SHIPPED_DWELL_MS, SHIPPED_DWELL_MS, true);
    focusCoverageNote(c, BIN, SHIPPED_DWELL_MS, SHIPPED_DWELL_MS, true);
    TEST_ASSERT_EQUAL_UINT16(1, c.valid_passes);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::SAMPLED, focusCoverageLabelFor(c));
}

void test_observation_time_saturates_rather_than_wraps() {
    // An operator who leaves Focus running should see coverage stop climbing,
    // not roll back to insufficient.
    FocusCoverage c = fresh();
    focusCoverageNote(c, BIN, SHIPPED_DWELL_MS, UINT32_MAX - 10, true);
    focusCoverageNote(c, BIN, SHIPPED_DWELL_MS, 1000, true);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c.observation_ms);
    TEST_ASSERT_EQUAL(FocusCoverageLabel::SAMPLED, focusCoverageLabelFor(c));
}

void test_thresholds_are_ordered_and_reachable_by_the_shipped_dwell() {
    // The shipped request is a 2,000 ms pass; if the constants ever move so
    // that one press cannot reach `sampled`, that is a product change and this
    // should fail rather than silently downgrade every result.
    TEST_ASSERT_TRUE(FOCUS_MIN_OBSERVATION_MS <= SHIPPED_DWELL_MS);
    TEST_ASSERT_TRUE(FOCUS_MIN_VALID_PASSES >= 1);
    TEST_ASSERT_TRUE(FOCUS_REPEATED_VALID_PASSES >= FOCUS_MIN_VALID_PASSES);
    TEST_ASSERT_TRUE(FOCUS_REPEATED_OBSERVATION_MS >= FOCUS_MIN_OBSERVATION_MS);
    // repeated must be reachable by whole shipped passes, not just in principle
    TEST_ASSERT_EQUAL_UINT32(0, FOCUS_REPEATED_OBSERVATION_MS % SHIPPED_DWELL_MS);
    TEST_ASSERT_TRUE(SHIPPED_DWELL_MS >= FOCUS_COVERAGE_MIN_DWELL_MS);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_observed_is_insufficient);
    RUN_TEST(test_one_shipped_pass_is_sampled);
    RUN_TEST(test_three_shipped_passes_are_repeated);
    RUN_TEST(test_both_halves_of_a_pair_must_be_met);
    RUN_TEST(test_short_passes_never_count);
    RUN_TEST(test_invalid_passes_never_count);
    RUN_TEST(test_changing_bin_resets_accumulated_coverage);
    RUN_TEST(test_returning_to_a_bin_does_not_restore_its_old_coverage);
    RUN_TEST(test_observation_time_saturates_rather_than_wraps);
    RUN_TEST(test_thresholds_are_ordered_and_reachable_by_the_shipped_dwell);
    return UNITY_END();
}
