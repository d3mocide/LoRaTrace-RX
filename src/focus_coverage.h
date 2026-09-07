#pragma once
// LoRaTrace RX — Focus coverage labels (Workstream 12 §3/§3.1).
//
// Coverage answers exactly one question: how much looking has this bin had?
// §3 constrains it hard — a label is "derived only from valid-pass count and
// accumulated observation time", and must not mean signal strength, likelihood
// of absence, or confidence in identity. It is emphatically not an activity
// claim: Focus reports coverage and never activity (2026-09-06 decision,
// docs/research/phase12-survey-truth-design.md §8 "Decisions").
//
// Pure logic, no Arduino dependency, so the thresholds are host-testable the
// same way focus_plan.h/focus_observation.h are — a threshold nobody can test
// is a threshold nobody can trust.

#include <stdint.h>
#include "channel_plans.h"

// --- The four constants §3.1 required be selected by measurement -----------
//
// Selected 2026-09-07 from the coverage campaign
// (docs/hardware-results/2026-09-06-phase12-coverage-campaign.md, 80 passes).
//
// The campaign's finding that made this a small decision: repeated passes are
// deterministic. Observation-time stdev was 0.0 ms and sample count invariant
// across every repeat at every dwell, so `passes x dwell` reproduces observation
// time exactly. **The pass thresholds and the millisecond thresholds are the
// same constraint stated twice** — they are kept as two pairs because §3.1 names
// four constants and because a future variable dwell would separate them, but
// today one follows from the other.
//
// Both halves of a pair must be met. With Focus's shipped 2,000 ms pass that
// makes `sampled` one press and `repeated` three.

// One deliberate look. Focus *is* a single bounded look at one bin, so
// requiring two before the device will describe what it saw would be strange.
constexpr uint16_t FOCUS_MIN_VALID_PASSES = 1;
constexpr uint32_t FOCUS_MIN_OBSERVATION_MS = 2000;

// More than once, on separate occasions. Three is judgement, not measurement:
// the campaign showed any threshold is equally achievable and equally
// predictable, so nothing in the data prefers 3 over 4. It is the smallest
// count that distinguishes a glance from a habit.
constexpr uint16_t FOCUS_REPEATED_VALID_PASSES = 3;
constexpr uint32_t FOCUS_REPEATED_OBSERVATION_MS = 6000;

// A pass shorter than this counts toward neither label. This one *is* from
// measurement, twice over: §6.4 found a short pass cannot both catch a source
// and reject ambient at any threshold, and the coverage campaign found the flat
// 74 ms per-pass overhead makes short passes cost more Watch time per second
// observed (10 s of observation costs 13.0 s of Watch at 250 ms passes against
// 10.4 s at 2,000 ms). Short passes are worse on both axes at once and
// repeating them fixes neither.
//
// Constrains nothing today — Focus ships a 2,000 ms dwell — so this is a guard
// against a future shorter pass quietly accumulating into a label it should not.
constexpr uint16_t FOCUS_COVERAGE_MIN_DWELL_MS = 500;

enum class FocusCoverageLabel : uint8_t {
    INSUFFICIENT = 0, // not enough looking to report the observation
    SAMPLED,          // enough looking to report what was seen
    REPEATED,         // looked at across multiple separated passes
};

inline const char *focusCoverageLabelName(FocusCoverageLabel label) {
    switch (label) {
        case FocusCoverageLabel::SAMPLED: return "sampled";
        case FocusCoverageLabel::REPEATED: return "repeated";
        default: return "insufficient";
    }
}

// No bin selected yet. 0xFFFF rather than 0, which is a real bin index.
constexpr uint16_t FOCUS_COVERAGE_NO_BIN = 0xFFFF;

// Accumulator for ONE bin. Coverage is defined across repeated requests, and a
// single result row cannot know about earlier ones — so the device carries this
// much state and no more.
//
// Scoped to the current bin and reset when the selection moves, which is both
// bounded (8 bytes, no per-bin table and no heap) and the honest reading of the
// contract: accumulated observation time means time on *that* frequency, so
// carrying it across a retune would overstate coverage of the new bin.
struct FocusCoverage {
    uint16_t bin_index = FOCUS_COVERAGE_NO_BIN;
    uint16_t valid_passes = 0;
    uint32_t observation_ms = 0;
    ChannelParams channel = {};
    uint32_t frequency_khz = 0;
};

inline void focusCoverageContext(FocusCoverage &coverage, uint32_t frequency_khz,
                                  const ChannelParams &channel) {
    if (coverage.frequency_khz != frequency_khz || coverage.channel.bw_khz != channel.bw_khz ||
        coverage.channel.sf != channel.sf || coverage.channel.cr_denom != channel.cr_denom ||
        coverage.channel.sync_word != channel.sync_word) {
        coverage = FocusCoverage{};
        coverage.frequency_khz = frequency_khz;
        coverage.channel = channel;
    }
}

// Fold one terminated request into the accumulator. `valid` is the caller's own
// judgement that the pass configured its frequency, produced its samples, and
// was neither cancelled nor radio-error terminated (§3's "valid pass").
inline void focusCoverageNote(FocusCoverage &coverage, uint16_t bin_index, uint16_t dwell_ms,
                              uint32_t observation_ms, bool valid) {
    if (coverage.bin_index != bin_index) {
        coverage.bin_index = bin_index;
        coverage.valid_passes = 0;
        coverage.observation_ms = 0;
    }
    if (!valid || dwell_ms < FOCUS_COVERAGE_MIN_DWELL_MS) return;
    if (coverage.valid_passes < UINT16_MAX) coverage.valid_passes++;
    // Saturate rather than wrap: an operator who leaves Focus running all day
    // should see coverage stop climbing, not roll back to insufficient.
    const uint32_t room = UINT32_MAX - coverage.observation_ms;
    coverage.observation_ms += (observation_ms > room) ? room : observation_ms;
}

inline FocusCoverageLabel focusCoverageLabelFor(const FocusCoverage &coverage) {
    if (coverage.valid_passes >= FOCUS_REPEATED_VALID_PASSES &&
        coverage.observation_ms >= FOCUS_REPEATED_OBSERVATION_MS) {
        return FocusCoverageLabel::REPEATED;
    }
    if (coverage.valid_passes >= FOCUS_MIN_VALID_PASSES &&
        coverage.observation_ms >= FOCUS_MIN_OBSERVATION_MS) {
        return FocusCoverageLabel::SAMPLED;
    }
    return FocusCoverageLabel::INSUFFICIENT;
}

// The thresholds have to be orderable or the labels are incoherent.
static_assert(FOCUS_REPEATED_VALID_PASSES >= FOCUS_MIN_VALID_PASSES,
              "repeated cannot need fewer passes than sampled");
static_assert(FOCUS_REPEATED_OBSERVATION_MS >= FOCUS_MIN_OBSERVATION_MS,
              "repeated cannot need less observation time than sampled");
