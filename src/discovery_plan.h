#pragma once
// LoRaTrace RX — Phase 8 built-in DISCOVERY_SWEEP candidates.
//
// This is deliberately a pure, fixed-data layer. It gives the radio task a
// versioned candidate list without introducing persistent custom plans,
// heap allocation, or UI-owned radio access. The radio task owns acquisition
// and its bounded exit/recovery state machine.

#include <stddef.h>
#include <stdint.h>

#include "channel_plans.h"

constexpr uint8_t DISCOVERY_PLAN_VERSION = 1;
constexpr uint8_t DISCOVERY_NO_SLOT = 0xFF;

enum class DiscoveryCandidateKind : uint8_t {
    MESHTASTIC_STANDARD_PRESET,
    MESHCORE_UPSTREAM_DEFAULT,
    MESHCORE_CURRENT_US,
};

struct DiscoveryCandidate {
    ChannelParams channel;
    DiscoveryCandidateKind kind;
    uint8_t slot; // Meshtastic zero-based slot; DISCOVERY_NO_SLOT otherwise.
};

struct DiscoveryPlan {
    uint8_t version;
    const DiscoveryCandidate *candidates;
    uint8_t count;
};

// Meshtastic's US region is 902-928 MHz. Its current firmware calculates the
// slot count from the selected bandwidth and places each channel at the
// bandwidth-wide channel centre. Keep the formula here so Probe cannot drift
// from the upstream selection rule when a preset changes bandwidth.
constexpr uint8_t MESHTASTIC_US_SLOT_COUNT_250 = 104;
constexpr uint8_t MESHTASTIC_US_SLOT_COUNT_125 = 208;
constexpr uint8_t MESHTASTIC_US_SLOT_COUNT_500 = 52;

constexpr float meshtasticUsFrequencyForSlot(uint16_t slot, float bw_khz) {
    return 902.0f + (bw_khz / 2000.0f) + (slot * bw_khz / 1000.0f);
}

// Standard Meshtastic preset-name hashes are the source-backed channel
// anchors. The active home tuple is included so the radio task can uniformly
// skip the resolved home channel (including an operator override) at runtime.
// Coding-rate values follow the upstream preset table: 5 means CR 4/5.
constexpr DiscoveryCandidate MESHTASTIC_STANDARD_CANDIDATES[] = {
    {{meshtasticUsFrequencyForSlot(19, 250.0f), 11, 250.0f, 5, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 19}, // LongFast
    {{meshtasticUsFrequencyForSlot(86, 125.0f), 11, 125.0f, 8, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 86}, // LongModerate
    {{meshtasticUsFrequencyForSlot(26, 125.0f), 12, 125.0f, 8, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 26}, // LongSlow
    {{meshtasticUsFrequencyForSlot(44, 250.0f), 9, 250.0f, 5, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 44}, // MediumFast
    {{meshtasticUsFrequencyForSlot(51, 250.0f), 10, 250.0f, 5, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 51}, // MediumSlow
    {{meshtasticUsFrequencyForSlot(67, 250.0f), 7, 250.0f, 5, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 67}, // ShortFast
    {{meshtasticUsFrequencyForSlot(74, 250.0f), 8, 250.0f, 5, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 74}, // ShortSlow
    {{meshtasticUsFrequencyForSlot(13, 500.0f), 11, 500.0f, 8, SYNC_WORD_MESHTASTIC},
     DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET, 13}, // LongTurbo
};

// MeshCore has no Meshtastic-style channel-name slot hash. The first tuple is
// the current LoRa default in MeshCore's upstream companion example; the
// second is this firmware's source-backed US narrow-band home tuple. A
// pre-migration/legacy tuple is intentionally not guessed here: its coding
// rate is not established by the project's evidence rules.
constexpr DiscoveryCandidate MESHCORE_CANDIDATES[] = {
    {CHANNEL_MESHCORE_US_NARROW, DiscoveryCandidateKind::MESHCORE_CURRENT_US,
     DISCOVERY_NO_SLOT},
    {{915.0f, 10, 250.0f, 5, SYNC_WORD_MESHCORE},
     DiscoveryCandidateKind::MESHCORE_UPSTREAM_DEFAULT, DISCOVERY_NO_SLOT},
};

constexpr DiscoveryPlan EMPTY_DISCOVERY_PLAN = {
    DISCOVERY_PLAN_VERSION, nullptr, 0};

inline DiscoveryPlan discoveryPlanForProfile(MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHTASTIC:
            return {DISCOVERY_PLAN_VERSION, MESHTASTIC_STANDARD_CANDIDATES,
                    (uint8_t)(sizeof(MESHTASTIC_STANDARD_CANDIDATES) /
                              sizeof(MESHTASTIC_STANDARD_CANDIDATES[0]))};
        case MissionProfile::MESHCORE:
            return {DISCOVERY_PLAN_VERSION, MESHCORE_CANDIDATES,
                    (uint8_t)(sizeof(MESHCORE_CANDIDATES) /
                              sizeof(MESHCORE_CANDIDATES[0]))};
        case MissionProfile::RETICULUM:
        case MissionProfile::GENERAL_EXPLORATION:
        default:
            return EMPTY_DISCOVERY_PLAN;
    }
}

inline bool discoveryChannelEquals(const ChannelParams &a, const ChannelParams &b) {
    return a.freq_mhz == b.freq_mhz && a.sf == b.sf && a.bw_khz == b.bw_khz &&
           a.cr_denom == b.cr_denom && a.sync_word == b.sync_word;
}
