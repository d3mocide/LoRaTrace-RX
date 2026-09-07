#pragma once
#include "energy_plan.h"
#include "channel_plans.h"

struct SweepSnapshot {
    uint32_t generation = 0;
    uint32_t completed_ms = 0;
    Region region = Region::US;
    EnergyBinStep step = ENERGY_SWEEP_DEFAULT_STEP;
    ChannelParams channel = {};
    uint16_t bin_count = 0;
    uint16_t capture_bin = 0xFFFF;
    uint16_t captures = 0;
    bool complete = false;
    uint8_t peaks[28] = {};
    uint8_t sampled[28] = {};
};
inline bool sweepBit(const uint8_t *mask, uint16_t bin) {
    return bin < 224 && (mask[bin / 8] & (1u << (bin % 8)));
}
inline void sweepSetBit(uint8_t *mask, uint16_t bin) {
    if (bin < 224) mask[bin / 8] |= (1u << (bin % 8));
}
static_assert(sizeof(SweepSnapshot) <= 96, "Sweep snapshot budget exceeded");
