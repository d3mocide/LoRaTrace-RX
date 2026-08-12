#pragma once
// LoRaTrace RX — per-profile RF parameter tables. See DESIGN.md §3 for the
// full rationale and confidence assessment behind each value; don't
// re-derive these numbers from scratch or "improve" them without updating
// DESIGN.md first.
//
// Only profiles with source-backed concrete values get a ChannelParams
// constant. Profiles that are discovery problems by design (Reticulum,
// General Exploration), or whose parameters are still genuinely unverified
// (MeshCore legacy CR), stay as comments until the relevant build-order
// phase (DESIGN.md §9) needs them — see CLAUDE.md house rule against
// hardcoding unverified sync words/params.

#include <cstdint>

enum class MissionProfile : uint8_t {
    MESHTASTIC,
    MESHCORE,
    RETICULUM,
    GENERAL_EXPLORATION,
};

struct ChannelParams {
    float freq_mhz;
    uint8_t sf;
    float bw_khz;
    uint8_t cr_denom; // e.g. 8 means coding rate 4/8
};

// Meshtastic — US LongFast default, slot 20 of 104. HIGH confidence
// (official docs + multiple corroborating sources, DESIGN.md §3).
//
// This is ONE slot out of a 104-slot hash space, not full protocol
// coverage — non-default channel names land on other slots and are only
// caught by DISCOVERY_SWEEP (DESIGN.md §4, build-order phase 4).
constexpr ChannelParams CHANNEL_MESHTASTIC_LONGFAST_US = {
    .freq_mhz = 906.875f,
    .sf = 11,
    .bw_khz = 250.0f,
    .cr_denom = 8,
};

// MeshCore — US/Canada "Recommended" preset, post-Oct-2025 narrow-BW
// migration. HIGH confidence (MeshCore's own FAQ + multiple community
// sites agree, DESIGN.md §3). MeshCore has no slot-hashing scheme — one
// frequency covers the whole regional community (DESIGN.md §3).
constexpr ChannelParams CHANNEL_MESHCORE_US_NARROW = {
    .freq_mhz = 910.525f,
    .sf = 7,
    .bw_khz = 62.5f,
    .cr_denom = 5,
};

// --- Not yet parameterized — build-order phase 3/4, DESIGN.md §3/§9 ---
//
// MeshCore legacy (pre-migration, may still be in the wild): ~915MHz,
// SF11, BW250. Coding rate is genuinely unspecified in DESIGN.md ("—" in
// the table) — do not guess a value here. MEDIUM confidence; treat as a
// DISCOVERY_SWEEP candidate, not a HOME_LISTEN target, until CR is sourced.
//
// Reticulum: no fixed target by design. Communities deliberately pick
// arbitrary settings offset from Meshtastic/MeshCore to avoid collision.
// Handled entirely by ENERGY_SWEEP (build-order phase 5) — never gets a
// ChannelParams entry.
//
// General Exploration: full 868-923MHz sweep, any SF/BW/CR. Also an
// ENERGY_SWEEP consumer, not a fixed channel.

// Phase 1 (current) hardcodes CHANNEL_MESHTASTIC_LONGFAST_US directly in
// main.cpp. Don't route these tables through a live radio-task config
// until the task/queue architecture from DESIGN.md §9 step 2+ exists to
// own that responsibility — see CLAUDE.md Status.
