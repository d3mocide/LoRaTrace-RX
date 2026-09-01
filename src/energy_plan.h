#pragma once
// LoRaTrace RX — Phase 9 ENERGY_SWEEP frequency-bin math. Pure formula, not
// a curated candidate list like discovery_plan.h: Sweep's bins are one
// uniform grid, shared by RETICULUM and GENERAL_EXPLORATION alike.

#include <stdint.h>

#include "region_plan.h"

// M5Stack's specified Cap LoRa-1262 front end (docs/DESIGN.md §1) — the
// GLOBAL preset, no regional constraint. Existing raw 868.0f/923.0f
// literals in discovery_plan.h/channel_plans.h tests stay as-is, out of
// scope here.
struct EnergySweepBand {
    float lo_mhz;
    float hi_mhz;
};

constexpr EnergySweepBand ENERGY_SWEEP_BAND_GLOBAL = {868.0f, 923.0f};
// 47 CFR § 15.247's 902-928MHz US ISM band. High edge stays the same
// hardware ceiling as GLOBAL rather than the regulatory 928MHz top — this
// module's own tuned front end already caps out at 923MHz (docs/DESIGN.md
// §1, the same "923-928MHz front-end rolloff" gap tracked in
// docs/STATUS.md), so only the low edge actually moves for US.
constexpr EnergySweepBand ENERGY_SWEEP_BAND_US = {902.0f, 923.0f};

constexpr EnergySweepBand energySweepBandForRegion(Region region) {
    return region == Region::US ? ENERGY_SWEEP_BAND_US : ENERGY_SWEEP_BAND_GLOBAL;
}

// research/...Phases-7-10-Design.md §7.3: "a sensible starting experiment
// is 250 kHz or 500 kHz, not a frozen requirement" — an enum (not a raw
// uint16_t) so a later on-device preset picker can't select an untested
// step.
enum class EnergyBinStep : uint16_t {
    KHZ_250 = 250,
    KHZ_500 = 500,
};

// §7.3 gives 250kHz's worked example (221 bins, ~8B/bin, <1.8KB) as the
// reference number, so it's the default; 500kHz stays a fully supported,
// equally tested alternative.
constexpr EnergyBinStep ENERGY_SWEEP_DEFAULT_STEP = EnergyBinStep::KHZ_250;

// §7.3: "reserve at most 224 bins" — a hard ceiling independent of step
// choice; also why EnergyObservation's bin_index (below) is safely uint8_t.
// GLOBAL's range is the larger case at any step (US is a subset), so this
// ceiling only needs checking against GLOBAL below.
constexpr uint16_t ENERGY_BIN_RESERVED_COUNT = 224;

constexpr uint16_t energyBinStepKhz(EnergyBinStep step) {
    return (uint16_t)step;
}

constexpr uint16_t energyBinCount(const EnergySweepBand &band, EnergyBinStep step) {
    return (uint16_t)((uint32_t)((band.hi_mhz - band.lo_mhz) * 1000.0f) /
                       energyBinStepKhz(step)) + 1;
}

constexpr float energyBinFrequencyMhz(uint16_t bin_index, const EnergySweepBand &band,
                                       EnergyBinStep step) {
    return band.lo_mhz + (float)bin_index * (float)energyBinStepKhz(step) / 1000.0f;
}

// Nearest bin to freq_mhz, clamped into [0, count-1] rather than
// out-of-range — a caller handing in a slightly-off frequency (float
// rounding, a UI-selected bin re-derived from its displayed MHz value)
// gets the closest real bin, not undefined behavior.
inline uint16_t energyBinIndexForFrequencyMhz(float freq_mhz, const EnergySweepBand &band,
                                               EnergyBinStep step) {
    const uint16_t count = energyBinCount(band, step);
    if (freq_mhz <= band.lo_mhz) return 0;
    if (freq_mhz >= band.hi_mhz) return (uint16_t)(count - 1);
    const float offset_khz = (freq_mhz - band.lo_mhz) * 1000.0f;
    uint16_t idx = (uint16_t)(offset_khz / (float)energyBinStepKhz(step) + 0.5f);
    if (idx >= count) idx = (uint16_t)(count - 1);
    return idx;
}

static_assert(energyBinCount(ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250) == 221,
              "250kHz bin count must match docs/DESIGN.md §7.3's worked example");
static_assert(energyBinCount(ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_250) <=
                  ENERGY_BIN_RESERVED_COUNT,
              "250kHz bin count exceeds the reserved ceiling");
static_assert(energyBinCount(ENERGY_SWEEP_BAND_GLOBAL, EnergyBinStep::KHZ_500) <=
                  ENERGY_BIN_RESERVED_COUNT,
              "500kHz bin count exceeds the reserved ceiling");
