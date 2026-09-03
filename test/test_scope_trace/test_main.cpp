#include <unity.h>

#include "../../src/scope_trace.h"

void test_budget_ceiling_matches_design_doc() {
    TEST_ASSERT_EQUAL_UINT16(240, SCOPE_MAX_SAMPLES);
}

void test_encode_decode_round_trip_and_clamp() {
    TEST_ASSERT_EQUAL_INT8(-90, scopeEncodeRssiDbm(-90.4f));
    TEST_ASSERT_EQUAL_INT8(-91, scopeEncodeRssiDbm(-90.6f));
    TEST_ASSERT_EQUAL_INT8(-128, scopeEncodeRssiDbm(-200.0f)); // clamps, doesn't wrap
    TEST_ASSERT_EQUAL_INT8(127, scopeEncodeRssiDbm(500.0f));
    TEST_ASSERT_EQUAL_FLOAT(-90.0f, scopeDecodeRssiDbm(-90));
}

void test_reset_clears_readable_history() {
    ScopeTrace trace;
    scopeTracePush(trace, -80.0f);
    scopeTraceReset(trace, 915.0f, 20, 1000);
    TEST_ASSERT_EQUAL_UINT16(0, trace.count);
    int8_t value;
    TEST_ASSERT_FALSE(scopeTraceSampleAt(trace, 0, value));
    TEST_ASSERT_EQUAL_FLOAT(915.0f, trace.tuned_freq_mhz);
    TEST_ASSERT_EQUAL_UINT16(20, trace.sample_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, trace.start_millis);
}

void test_push_and_recency_order() {
    ScopeTrace trace;
    scopeTraceReset(trace, 915.0f, 20, 0);
    scopeTracePush(trace, -90.0f);
    scopeTracePush(trace, -80.0f);
    scopeTracePush(trace, -70.0f);

    int8_t value;
    TEST_ASSERT_TRUE(scopeTraceSampleAt(trace, 0, value));
    TEST_ASSERT_EQUAL_INT8(-70, value);
    TEST_ASSERT_TRUE(scopeTraceSampleAt(trace, 2, value));
    TEST_ASSERT_EQUAL_INT8(-90, value);
    TEST_ASSERT_FALSE(scopeTraceSampleAt(trace, 3, value));
}

void test_ring_wraps_and_evicts_oldest() {
    ScopeTrace trace;
    scopeTraceReset(trace, 915.0f, 20, 0);
    for (uint16_t i = 0; i < SCOPE_MAX_SAMPLES + 5; i++) {
        scopeTracePush(trace, -(float)(100 + i));
    }
    TEST_ASSERT_EQUAL_UINT16(SCOPE_MAX_SAMPLES, trace.count);
    int8_t newest, oldest;
    TEST_ASSERT_TRUE(scopeTraceSampleAt(trace, 0, newest));
    TEST_ASSERT_TRUE(scopeTraceSampleAt(trace, SCOPE_MAX_SAMPLES - 1, oldest));
    TEST_ASSERT_EQUAL_INT8(scopeEncodeRssiDbm(-(float)(100 + SCOPE_MAX_SAMPLES + 4)), newest);
    TEST_ASSERT_EQUAL_INT8(scopeEncodeRssiDbm(-(float)(100 + 5)), oldest); // 5 oldest evicted
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_ceiling_matches_design_doc);
    RUN_TEST(test_encode_decode_round_trip_and_clamp);
    RUN_TEST(test_reset_clears_readable_history);
    RUN_TEST(test_push_and_recency_order);
    RUN_TEST(test_ring_wraps_and_evicts_oldest);
    return UNITY_END();
}
