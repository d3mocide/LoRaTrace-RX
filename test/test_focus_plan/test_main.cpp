#include <unity.h>

#include "../../src/focus_plan.h"

void test_focus_request_is_small_and_starts_with_one_selected_bin() {
    TEST_ASSERT_EQUAL_UINT8(1, FOCUS_SELECTED_BIN_COUNT);
    TEST_ASSERT_TRUE(sizeof(FocusRequest) <= 16);
}

void test_focus_selection_sources_are_explicit_and_stable() {
    TEST_ASSERT_EQUAL_STRING("sweep", focusSelectionSourceName(FocusSelectionSource::SWEEP_BIN));
    TEST_ASSERT_EQUAL_STRING("waterfall", focusSelectionSourceName(FocusSelectionSource::WATERFALL_BIN));
    TEST_ASSERT_EQUAL_STRING("preset", focusSelectionSourceName(FocusSelectionSource::PRESET));
}

void test_focus_request_resolves_a_sourced_energy_bin_frequency() {
    FocusRequest request;
    request.region = Region::US;
    request.bin_step = EnergyBinStep::KHZ_250;
    request.selection_bin_index = 13;
    request.requested_dwell_ms = 100;
    request.requested_samples = 4;
    request.requested_passes = FOCUS_BENCH_REQUESTED_PASSES;
    request.selection_source = FocusSelectionSource::SWEEP_BIN;

    TEST_ASSERT_TRUE(focusRequestIsValid(request));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 905.25f, focusRequestFrequencyMhz(request));
}

void test_focus_request_rejects_empty_or_out_of_band_work() {
    FocusRequest request;
    request.region = Region::US;
    request.bin_step = EnergyBinStep::KHZ_250;
    request.selection_bin_index = energyBinCount(ENERGY_SWEEP_BAND_US, request.bin_step);
    request.requested_dwell_ms = 100;
    request.requested_samples = FOCUS_BENCH_SAMPLES_MIN;
    request.requested_passes = FOCUS_BENCH_REQUESTED_PASSES;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));

    request.selection_bin_index = 0;
    request.requested_dwell_ms = FOCUS_BENCH_DWELL_MIN_MS - 1;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));
    request.requested_dwell_ms = FOCUS_BENCH_DWELL_MAX_MS + 1;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));
    request.requested_dwell_ms = 100;
    request.requested_samples = FOCUS_BENCH_SAMPLES_MIN - 1;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));
    request.requested_samples = FOCUS_BENCH_SAMPLES_MIN;
    request.requested_passes = 0;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));
}

void test_focus_request_is_bounded_in_time_not_only_in_samples() {
    FocusRequest request;
    request.region = Region::US;
    request.bin_step = EnergyBinStep::KHZ_250;
    request.selection_bin_index = 43;
    request.requested_dwell_ms = 500;
    request.requested_samples = 8;
    request.requested_passes = FOCUS_BENCH_REQUESTED_PASSES;
    request.selection_source = FocusSelectionSource::SWEEP_BIN;

    TEST_ASSERT_TRUE(focusRequestIsValid(request));
    // The deadline must exceed the dwell it bounds, or a healthy request
    // would report `timeout` instead of what it actually observed.
    TEST_ASSERT_TRUE(focusRequestTimeoutMs(request) > request.requested_dwell_ms);
    TEST_ASSERT_EQUAL_UINT32(500u + FOCUS_REQUEST_TIMEOUT_SLACK_MS,
                             focusRequestTimeoutMs(request));

    request.requested_dwell_ms = FOCUS_BENCH_DWELL_MAX_MS;
    TEST_ASSERT_EQUAL_UINT32(FOCUS_MAX_SAMPLING_MS, focusRequestTimeoutMs(request));
    TEST_ASSERT_TRUE(FOCUS_MAX_SAMPLING_MS <= 3000);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_focus_request_is_small_and_starts_with_one_selected_bin);
    RUN_TEST(test_focus_selection_sources_are_explicit_and_stable);
    RUN_TEST(test_focus_request_resolves_a_sourced_energy_bin_frequency);
    RUN_TEST(test_focus_request_rejects_empty_or_out_of_band_work);
    RUN_TEST(test_focus_request_is_bounded_in_time_not_only_in_samples);
    return UNITY_END();
}
