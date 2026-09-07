// Audit A22. Counting operations is not a bound on radio-away time: Scope's
// 240 samples at 20 ms read as 5 seconds and could take minutes, because every
// sample may wait on the SPI bus and again on a display mutex. These tests
// cover the wall-clock contract that replaces the count, including the
// rollover the device will actually hit after 49.7 days.

#include <unity.h>

#include "../../src/action_budget.h"

void setUp(void) {}
void tearDown(void) {}

void test_acquisition_and_total_are_separate_deadlines() {
    // Restore gets its own allowance: a restore running late is still worth
    // finishing, because abandoning it leaves the radio deaf.
    const ActionBudget b = actionBudgetBegin(1000, 5000, 2000);
    TEST_ASSERT_EQUAL_UINT32(6000, b.acquisition_deadline_ms);
    TEST_ASSERT_EQUAL_UINT32(8000, b.total_deadline_ms);

    TEST_ASSERT_FALSE(actionBudgetAcquisitionExpired(b, 5999));
    TEST_ASSERT_TRUE(actionBudgetAcquisitionExpired(b, 6000));
    TEST_ASSERT_FALSE(actionBudgetTotalExpired(b, 7999));
    TEST_ASSERT_TRUE(actionBudgetTotalExpired(b, 8000));
}

void test_remaining_saturates_instead_of_wrapping() {
    // Used as a per-operation timeout, so an expired budget must yield 0 and
    // not four billion milliseconds.
    const ActionBudget b = actionBudgetBegin(1000, 5000, 2000);
    TEST_ASSERT_EQUAL_UINT32(5000, actionBudgetRemainingMs(b, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, actionBudgetRemainingMs(b, 5999));
    TEST_ASSERT_EQUAL_UINT32(0, actionBudgetRemainingMs(b, 6000));
    TEST_ASSERT_EQUAL_UINT32(0, actionBudgetRemainingMs(b, 60000));
}

void test_deadlines_survive_the_millis_rollover() {
    // Starting 1 s before the uint32 wrap: an unsigned comparison would call
    // this expired immediately and abandon every action for 49.7 days.
    const uint32_t nearWrap = 0xFFFFFC18u; // ~1000 ms before wrap
    const ActionBudget b = actionBudgetBegin(nearWrap, 5000, 2000);

    TEST_ASSERT_FALSE(actionBudgetAcquisitionExpired(b, nearWrap + 100));
    TEST_ASSERT_FALSE(actionBudgetAcquisitionExpired(b, (uint32_t)(nearWrap + 4999)));
    TEST_ASSERT_TRUE(actionBudgetAcquisitionExpired(b, (uint32_t)(nearWrap + 5000)));
    TEST_ASSERT_TRUE(actionBudgetTotalExpired(b, (uint32_t)(nearWrap + 7000)));

    TEST_ASSERT_EQUAL_UINT32(4900, actionBudgetRemainingMs(b, nearWrap + 100));
    TEST_ASSERT_EQUAL_UINT32(0, actionBudgetRemainingMs(b, (uint32_t)(nearWrap + 6000)));
    TEST_ASSERT_EQUAL_UINT32(6000, actionBudgetElapsedMs(b, (uint32_t)(nearWrap + 6000)));
}

void test_a_late_sample_is_skipped_not_taken_immediately() {
    // The Focus catch-up burst: samples scheduled 20 ms apart, delayed by
    // contention, then fired back to back so the row claims a spacing the
    // measurement never had.
    TEST_ASSERT_TRUE(actionSampleIsOnTime(1000, 1000, 5));
    TEST_ASSERT_TRUE(actionSampleIsOnTime(1000, 1005, 5));
    TEST_ASSERT_FALSE(actionSampleIsOnTime(1000, 1006, 5));
    // Early is always fine — the caller waits for the target.
    TEST_ASSERT_TRUE(actionSampleIsOnTime(1000, 900, 5));
    // ...and lateness is measured across the rollover too.
    TEST_ASSERT_FALSE(actionSampleIsOnTime(0xFFFFFFF0u, 10, 5));
    TEST_ASSERT_TRUE(actionSampleIsOnTime(0xFFFFFFF0u, 0xFFFFFFF2u, 5));
}

void test_a_zero_budget_is_expired_immediately() {
    // Guards against a caller computing 0 from a bad setting and then looping
    // forever because "expired" was defined as strictly greater.
    const ActionBudget b = actionBudgetBegin(1000, 0, 0);
    TEST_ASSERT_TRUE(actionBudgetAcquisitionExpired(b, 1000));
    TEST_ASSERT_TRUE(actionBudgetTotalExpired(b, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, actionBudgetRemainingMs(b, 1000));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_acquisition_and_total_are_separate_deadlines);
    RUN_TEST(test_remaining_saturates_instead_of_wrapping);
    RUN_TEST(test_deadlines_survive_the_millis_rollover);
    RUN_TEST(test_a_late_sample_is_skipped_not_taken_immediately);
    RUN_TEST(test_a_zero_budget_is_expired_immediately);
    return UNITY_END();
}
