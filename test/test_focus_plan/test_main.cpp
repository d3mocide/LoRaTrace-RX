#include <unity.h>

#include "../../src/focus_observation.h"

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
    request.requested_samples = FOCUS_BENCH_SAMPLES_MAX + 1;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));
    request.requested_samples = FOCUS_BENCH_SAMPLES_MIN;
    request.requested_passes = 0;
    TEST_ASSERT_FALSE(focusRequestIsValid(request));
}

void test_sample_ceiling_keeps_every_quantile_exact() {
    // All samples landing in one 1 dB bucket is the worst case; above 255 the
    // bucket saturates and the histogram refuses to report a percentile.
    FocusRssiHistogram histogram;
    for (uint16_t i = 0; i < FOCUS_BENCH_SAMPLES_MAX; ++i) {
        focusHistogramAddSample(histogram, -1000);
    }
    TEST_ASSERT_TRUE(focusHistogramHasExactQuantiles(histogram));
    TEST_ASSERT_EQUAL_INT16(-1000, focusHistogramP90DbmX10(histogram));
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

void test_sampling_policy_scales_with_dwell_and_stays_bounded() {
    // The measured policy: spacing stays fixed, so the count follows the dwell.
    TEST_ASSERT_EQUAL_UINT16(6, focusSamplesForDwell(100));
    TEST_ASSERT_EQUAL_UINT16(26, focusSamplesForDwell(500));
    TEST_ASSERT_EQUAL_UINT16(101, focusSamplesForDwell(2000));

    // Actual spacing must never exceed the policy at any legal dwell -- that
    // is the property the 2026-09-04 sweep bought, not the sample counts.
    for (uint16_t dwell = FOCUS_BENCH_DWELL_MIN_MS; dwell <= FOCUS_BENCH_DWELL_MAX_MS; ++dwell) {
        const uint16_t samples = focusSamplesForDwell(dwell);
        TEST_ASSERT_TRUE(samples >= FOCUS_BENCH_SAMPLES_MIN);
        TEST_ASSERT_TRUE(samples <= FOCUS_BENCH_SAMPLES_MAX);
        TEST_ASSERT_TRUE(dwell / (samples - 1) <= FOCUS_SAMPLE_SPACING_MS);
    }
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_focus_request_is_small_and_starts_with_one_selected_bin);
    RUN_TEST(test_focus_selection_sources_are_explicit_and_stable);
    RUN_TEST(test_focus_request_resolves_a_sourced_energy_bin_frequency);
    RUN_TEST(test_focus_request_rejects_empty_or_out_of_band_work);
    RUN_TEST(test_focus_request_is_bounded_in_time_not_only_in_samples);
    RUN_TEST(test_sample_ceiling_keeps_every_quantile_exact);
    RUN_TEST(test_sampling_policy_scales_with_dwell_and_stays_bounded);
    return UNITY_END();
}
