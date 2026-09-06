#pragma once
// LoRaTrace RX — Phase 12 Focus Survey request contract. This is pure
// selection math only: it does not own the radio or start an acquisition.

#include <stdint.h>

#include "energy_plan.h"

// Phase 12 starts with exactly one selected frequency. A wider request needs
// a new static-RAM and radio-away budget; it must not quietly grow here.
constexpr uint8_t FOCUS_SELECTED_BIN_COUNT = 1;

// No maximum radio-away budget is enforced, and that is a decision rather than
// an omission (2026-09-06, docs/research/phase12-survey-truth-design.md §8
// "Decisions"). The cost is measured and linear in away time, Activity's
// AWAY T card already shows it, and one Enter is one pass — so an operator
// self-governs against a number on screen, and a cap would add a refusal path
// and a menu control for a runaway that cannot presently happen.
//
// READ THIS BEFORE ADDING REPEAT. The decision is conditional: Focus is a
// deliberate use, not a runtime state, and the self-governing argument rests
// entirely on a human pressing the key each time. If Focus ever gains an
// automatic repeat like Sweep's R binding, the away budget must be settled
// before that ships.
// The first bench prototype makes one measurement per request. The controlled
// matrix supplies its 30 trials by issuing 30 independently logged requests,
// not by monopolizing Watch for a hidden multi-pass loop.
constexpr uint8_t FOCUS_BENCH_REQUESTED_PASSES = 1;
constexpr uint16_t FOCUS_BENCH_DWELL_MIN_MS = 2;
constexpr uint16_t FOCUS_BENCH_DWELL_MAX_MS = 2000;
constexpr uint16_t FOCUS_BENCH_SAMPLES_MIN = 2;
// Raised from 64 to sweep sample spacing at a fixed dwell: the 2026-09-04
// matrix showed detection tracks the source's airtime against
// dwell/(samples-1), not against dwell, so selecting a real sampling policy
// needs spacings down to ~10 ms across a 2,000 ms dwell (201 samples).
//
// The ceiling is set by the histogram, not by RAM: bucket_counts are uint8_t,
// so a pass whose samples all land in one 1 dB bucket saturates at 255 and
// focusHistogramHasExactQuantiles() then refuses to report a percentile at
// all. Staying below that keeps every quantile exact in the worst case. The
// histogram itself is a fixed 141 bytes regardless of sample count, so this
// costs no additional static RAM (docs/research/phase12-survey-truth-design.md
// §4.2).
constexpr uint16_t FOCUS_BENCH_SAMPLES_MAX = 208;
static_assert(FOCUS_BENCH_SAMPLES_MAX < 255,
              "a pass must not be able to saturate a uint8 histogram bucket");

// How far apart a pass's RSSI samples may fall, in ms. This is the constant
// that decides what a pass can observe at all, and it was measured, not
// chosen: at a fixed 2,000 ms dwell, a 94 ms source was missed at 286 ms and
// 100 ms spacing (8/15 and 2/15 trials) and caught 15/15 at both 50 ms and
// 20 ms, with the worst-case reading improving from -97 dBm to -66 dBm
// between those two (docs/hardware-results/2026-09-04-phase12-focus-matrix.md).
// Detection collapses once spacing approaches the source's airtime, so 20 ms
// keeps roughly a 2x margin against the ~40-50 ms airtime of the fastest
// realistic mesh traffic.
//
// Finer sampling is free in the only currency that matters here: measured
// radio-away time was 2,073-2,075 ms across every arm, whether the pass took
// 8 samples or 101. The cost of a longer dwell is the dwell itself.
constexpr uint16_t FOCUS_SAMPLE_SPACING_MS = 20;

// Samples a dwell should take under that policy. Fixing the sample count
// instead -- as the first bench slice did at 8 -- makes a longer dwell
// strictly worse at catching bursts, because the spacing grows with it: a
// 2,000 ms dwell then observes eight instants, not 2,000 ms, which is exactly
// what the coverage vocabulary must never imply (§3).
// Rounds the interval count up, so actual spacing is at most the policy
// rather than at least it -- truncating would let a 30 ms dwell take two
// samples 30 ms apart and quietly violate the constant it is derived from.
// Split in two because the device toolchain builds this as C++11 (pinned
// Arduino-ESP32 2.0.17), where a constexpr body must be a single return --
// the native test environment's newer standard accepts locals and will not
// catch it.
constexpr uint16_t focusDerivedSampleCount(uint16_t dwell_ms) {
    return (uint16_t)((dwell_ms + FOCUS_SAMPLE_SPACING_MS - 1) / FOCUS_SAMPLE_SPACING_MS + 1);
}

constexpr uint16_t focusSamplesForDwell(uint16_t dwell_ms) {
    return focusDerivedSampleCount(dwell_ms) < FOCUS_BENCH_SAMPLES_MIN
               ? FOCUS_BENCH_SAMPLES_MIN
               : focusDerivedSampleCount(dwell_ms);
}

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

static_assert(focusSamplesForDwell(FOCUS_BENCH_DWELL_MAX_MS) <= FOCUS_BENCH_SAMPLES_MAX,
              "the sampling policy must stay inside the request's sample bound");

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
