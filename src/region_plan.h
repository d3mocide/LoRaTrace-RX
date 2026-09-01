#pragma once
// LoRaTrace RX — the operator's chosen regulatory region. Currently
// consumed only by ENERGY_SWEEP's band selection (energy_plan.h); Cell's
// FCC-specific framing and channel_plans.h's per-profile frequency
// tables are explicit, larger follow-ups (docs/ROADMAP.md), not wired to
// this yet.

#include <stdint.h>

enum class Region : uint8_t {
    GLOBAL, // no regional constraint — the module's full tuned range
    US,     // 47 CFR § 15.247's 902-928MHz US ISM band
};

constexpr const char *regionLabel(Region region) {
    return region == Region::US ? "US" : "Global";
}
