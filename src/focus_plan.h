#pragma once
// LoRaTrace RX — Phase 12 Focus Survey request contract. This is pure
// selection math only: it does not own the radio or start an acquisition.

#include <stdint.h>

#include "energy_plan.h"

// Phase 12 starts with exactly one selected frequency. A wider request needs
// a new static-RAM and radio-away budget; it must not quietly grow here.
constexpr uint8_t FOCUS_SELECTED_BIN_COUNT = 1;
// The first bench prototype makes one measurement per request. The controlled
// matrix supplies its 30 trials by issuing 30 independently logged requests,
// not by monopolizing Watch for a hidden multi-pass loop.
constexpr uint8_t FOCUS_BENCH_REQUESTED_PASSES = 1;
constexpr uint16_t FOCUS_BENCH_DWELL_MIN_MS = 2;
constexpr uint16_t FOCUS_BENCH_DWELL_MAX_MS = 2000;
constexpr uint16_t FOCUS_BENCH_SAMPLES_MIN = 2;
constexpr uint16_t FOCUS_BENCH_SAMPLES_MAX = 64;

// A bounded request must be bounded in wall-clock time, not just in samples:
// each sample waits on the shared SPI bus (radio_task.cpp's 250ms BUS_WAIT),
// so contention can stretch a nominal dwell well past it while the radio is
// away from home. Past this deadline the request stops sampling and
// terminates as `timeout` instead of running long. The slack absorbs a few
// bus waits plus scheduling jitter; it is a bench-slice bound, and §6's
// matrix (docs/research/phase12-survey-truth-design.md) may revise it before
// any operator-facing control exists.
constexpr uint16_t FOCUS_REQUEST_TIMEOUT_SLACK_MS = 1000;

enum class FocusSelectionSource : uint8_t {
    SWEEP_BIN = 0,
    WATERFALL_BIN,
    PRESET,
};

inline const char *focusSelectionSourceName(FocusSelectionSource source) {
    switch (source) {
        case FocusSelectionSource::SWEEP_BIN: return "sweep";
        case FocusSelectionSource::WATERFALL_BIN: return "waterfall";
        case FocusSelectionSource::PRESET: return "preset";
        default: return "unknown";
    }
}

constexpr bool focusSelectionSourceIsKnown(FocusSelectionSource source) {
    return source == FocusSelectionSource::SWEEP_BIN ||
           source == FocusSelectionSource::WATERFALL_BIN ||
           source == FocusSelectionSource::PRESET;
}

// The 100ms/500ms/2s bench arms are not production defaults, and coverage
// thresholds remain unselected. Bounds make this bench-only request finite.
struct FocusRequest {
    Region region = Region::GLOBAL;
    EnergyBinStep bin_step = ENERGY_SWEEP_DEFAULT_STEP;
    uint16_t selection_bin_index = 0;
    uint16_t requested_dwell_ms = 0;
    uint16_t requested_samples = 0;
    uint8_t requested_passes = 0;
    FocusSelectionSource selection_source = FocusSelectionSource::PRESET;
};

static_assert(sizeof(FocusRequest) <= 16,
              "FocusRequest must stay a small fixed radio-control payload");

constexpr bool focusRequestHasValidBin(const FocusRequest &request) {
    return request.selection_bin_index <
           energyBinCount(energySweepBandForRegion(request.region), request.bin_step);
}

constexpr bool focusRequestIsValid(const FocusRequest &request) {
    return FOCUS_SELECTED_BIN_COUNT == 1 &&
           focusSelectionSourceIsKnown(request.selection_source) &&
           focusRequestHasValidBin(request) &&
           request.requested_passes == FOCUS_BENCH_REQUESTED_PASSES &&
           request.requested_dwell_ms >= FOCUS_BENCH_DWELL_MIN_MS &&
           request.requested_dwell_ms <= FOCUS_BENCH_DWELL_MAX_MS &&
           request.requested_samples >= FOCUS_BENCH_SAMPLES_MIN &&
           request.requested_samples <= FOCUS_BENCH_SAMPLES_MAX;
}

// Deadline measured from the first sample-loop tick, not from the request's
// acceptance: queue latency is Core 1's scheduling, not the dwell's cost.
constexpr uint32_t focusRequestTimeoutMs(const FocusRequest &request) {
    return (uint32_t)request.requested_dwell_ms + FOCUS_REQUEST_TIMEOUT_SLACK_MS;
}

// Worst-case time a legal request may hold the radio before restore begins.
// Restore is bounded separately by restoreHomeListen()'s own path.
constexpr uint32_t FOCUS_MAX_SAMPLING_MS =
    (uint32_t)FOCUS_BENCH_DWELL_MAX_MS + FOCUS_REQUEST_TIMEOUT_SLACK_MS;
static_assert(FOCUS_MAX_SAMPLING_MS <= 3000,
              "Focus must not grow an unbudgeted radio-away window");

inline float focusRequestFrequencyMhz(const FocusRequest &request) {
    return energyBinFrequencyMhz(request.selection_bin_index,
                                  energySweepBandForRegion(request.region),
                                  request.bin_step);
}
