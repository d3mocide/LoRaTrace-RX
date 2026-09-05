#include <unity.h>

#include "../../src/focus_runtime.h"

FocusRequest validRequest() {
    FocusRequest request;
    request.region = Region::US;
    request.selection_bin_index = 13;
    request.requested_dwell_ms = 100;
    request.requested_samples = 4;
    request.requested_passes = FOCUS_BENCH_REQUESTED_PASSES;
    return request;
}

void test_focus_runtime_only_starts_a_bounded_valid_request() {
    FocusRuntime runtime;
    TEST_ASSERT_TRUE(focusRuntimeBegin(runtime, validRequest()));
    TEST_ASSERT_EQUAL((int)FocusRuntimeState::SURVEYING, (int)runtime.state);
    TEST_ASSERT_FALSE(focusRuntimeBegin(runtime, validRequest()));
}

void test_focus_runtime_publishes_complete_only_after_restore() {
    FocusRuntime runtime;
    TEST_ASSERT_TRUE(focusRuntimeBegin(runtime, validRequest()));
    focusRuntimeNoteValidPass(runtime, 93);
    focusRuntimeBeginRestore(runtime);
    TEST_ASSERT_EQUAL((int)FocusRequestStatus::COMPLETE,
                      (int)focusRuntimeFinishRestore(runtime, true));
    TEST_ASSERT_EQUAL_UINT16(1, runtime.valid_passes);
    TEST_ASSERT_EQUAL_UINT32(93, runtime.observation_ms);
}

void test_focus_runtime_restore_failure_overrides_cancel_and_timeout() {
    FocusRuntime runtime;
    TEST_ASSERT_TRUE(focusRuntimeBegin(runtime, validRequest()));
    focusRuntimeCancel(runtime);
    focusRuntimeTimeout(runtime);
    focusRuntimeBeginRestore(runtime);
    TEST_ASSERT_EQUAL((int)FocusRequestStatus::FAILED,
                      (int)focusRuntimeFinishRestore(runtime, false));
}

void test_focus_runtime_timeout_publishes_timeout_not_complete() {
    FocusRuntime runtime;
    TEST_ASSERT_TRUE(focusRuntimeBegin(runtime, validRequest()));
    focusRuntimeTimeout(runtime);
    focusRuntimeBeginRestore(runtime);
    // A recovered timeout is still a timeout: it must not borrow `complete`
    // from a restore that worked, and it recorded no valid pass.
    TEST_ASSERT_EQUAL((int)FocusRequestStatus::TIMEOUT,
                      (int)focusRuntimeFinishRestore(runtime, true));
    TEST_ASSERT_EQUAL_UINT16(0, runtime.valid_passes);
    TEST_ASSERT_EQUAL_UINT32(0, runtime.observation_ms);
}

void test_focus_runtime_cancel_outranks_a_concurrent_timeout() {
    FocusRuntime runtime;
    TEST_ASSERT_TRUE(focusRuntimeBegin(runtime, validRequest()));
    focusRuntimeCancel(runtime);
    focusRuntimeTimeout(runtime);
    focusRuntimeBeginRestore(runtime);
    TEST_ASSERT_EQUAL((int)FocusRequestStatus::CANCELLED,
                      (int)focusRuntimeFinishRestore(runtime, true));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_focus_runtime_only_starts_a_bounded_valid_request);
    RUN_TEST(test_focus_runtime_publishes_complete_only_after_restore);
    RUN_TEST(test_focus_runtime_restore_failure_overrides_cancel_and_timeout);
    RUN_TEST(test_focus_runtime_timeout_publishes_timeout_not_complete);
    RUN_TEST(test_focus_runtime_cancel_outranks_a_concurrent_timeout);
    return UNITY_END();
}
