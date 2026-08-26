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

// LoRa sync word. This is a *filter*, not cosmetic: the SX126x only raises
// an RX interrupt for packets whose sync word matches, so getting this
// wrong means hearing nothing from the target protocol while still hearing
// unrelated traffic that happens to match whatever was set. Passed to
// RadioLib's begin()/setSyncWord() as a single byte (RadioLib handles the
// SX126x two-byte register mapping internally).
//
// RadioLib's own default is RADIOLIB_SX126X_SYNC_WORD_PRIVATE == 0x12.
// Anything here without a source-verified value should stay on that
// default explicitly rather than borrowing another protocol's number.
constexpr uint8_t SYNC_WORD_RADIOLIB_DEFAULT = 0x12;

// Meshtastic 0x2B / MeshCore 0x12 — both verified against upstream firmware
// source (meshtastic/firmware `RadioLibInterface.h`, meshcore-dev/MeshCore
// `CustomSX1262.h`); full citations and the investigation history are
// DESIGN.md §3/§7's job, not this file's — don't re-derive it here.
// MeshCore's value is numerically identical to SYNC_WORD_RADIOLIB_DEFAULT
// but kept as its own named constant: they mean different things (one is
// "MeshCore's verified value," the other is "we don't know yet"), and a
// future MeshCore release could move off it.
constexpr uint8_t SYNC_WORD_MESHTASTIC = 0x2B;
constexpr uint8_t SYNC_WORD_MESHCORE = 0x12;

// Preamble length is deliberately NOT in this struct yet — RadioLib's
// default of 8 empirically does not block RX (confirmed decoding live
// Meshtastic frames on hardware), even though both protocols' own firmware
// uses 16. Left alone rather than "fixed": a continuous-RX receiver syncs
// on whatever preamble arrives, so this only starts to matter for the
// duty-cycled/CAD scanning in DESIGN.md §4 (phase 4+) — revisit there.
struct ChannelParams {
    float freq_mhz;
    uint8_t sf;
    float bw_khz;
    uint8_t cr_denom; // e.g. 8 means coding rate 4/8
    uint8_t sync_word;
};

// Meshtastic — US LongFast default, slot 20 of 104. HIGH confidence
// (official docs + multiple corroborating sources, DESIGN.md §3).
//
// This is ONE slot out of a 104-slot hash space, not full protocol
// coverage — non-default channel names land on other slots and are only
// caught by DISCOVERY_SWEEP (DESIGN.md §4, build-order phase 7).
constexpr ChannelParams CHANNEL_MESHTASTIC_LONGFAST_US = {
    .freq_mhz = 906.875f,
    .sf = 11,
    .bw_khz = 250.0f,
    .cr_denom = 8,
    .sync_word = SYNC_WORD_MESHTASTIC,
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
    .sync_word = SYNC_WORD_MESHCORE, // verified, see above — not a placeholder
};

// Maps a mission profile to its HOME_LISTEN channel table (DESIGN.md §5:
// "switching profiles reconfigures which channel table HOME_LISTEN...
// pull[s] from"). Only MESHTASTIC and MESHCORE have a concrete table today —
// RETICULUM and GENERAL_EXPLORATION are ENERGY_SWEEP-only by design (§3/§4,
// phase 8) and never get one, so they fall back to the Meshtastic default
// here rather than being a compile error a caller has to guard against.
inline ChannelParams channelParamsForProfile(MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHCORE: return CHANNEL_MESHCORE_US_NARROW;
        case MissionProfile::MESHTASTIC:
        default: return CHANNEL_MESHTASTIC_LONGFAST_US;
    }
}

// Per-profile SD/web overrides (config.h owns loading this from
// /loratrace/config.txt; wifi_task.cpp owns writing it). Two independent
// slots, not one shared ChannelParams — the whole point is that a
// Meshtastic override and a MeshCore override are different settings that
// must survive a profile switch independently of each other (found
// 2026-08-24: the single-slot design that predates this let a switch back
// to a profile silently drop its override and revert to the hardcoded
// default, see PROGRESS.md Decisions log). RETICULUM/GENERAL_EXPLORATION
// get no slot, same reason they get no ChannelParams constant above — they
// have no fixed HOME_LISTEN channel to override.
struct ProfileOverrides {
    bool meshtastic_set = false;
    ChannelParams meshtastic{};
    bool meshcore_set = false;
    ChannelParams meshcore{};
};

// Resolves the channel to actually use for `profile` right now: the
// operator's SD/web override if one was loaded, else the hardcoded default
// (channelParamsForProfile() above). Pure — no I/O, host-testable — so
// both the initial boot channel (main.cpp) and every later profile switch
// (radio_task.cpp's radioRequestProfileSwitch()) make this decision through
// the exact same function instead of each re-deriving it, which is what
// keeps them from drifting apart the way the pre-2026-08-24 design did.
inline ChannelParams resolvedChannelForProfile(const ProfileOverrides &overrides, MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHCORE:
            return overrides.meshcore_set ? overrides.meshcore : channelParamsForProfile(profile);
        case MissionProfile::MESHTASTIC:
        default:
            return overrides.meshtastic_set ? overrides.meshtastic : channelParamsForProfile(profile);
    }
}

// --- Not yet parameterized — build-order phase 5/6, DESIGN.md §3/§9 ---
//
// MeshCore legacy (pre-migration, may still be in the wild): ~915MHz,
// SF11, BW250. Coding rate is genuinely unspecified in DESIGN.md ("—" in
// the table) — do not guess a value here. MEDIUM confidence; treat as a
// DISCOVERY_SWEEP candidate, not a HOME_LISTEN target, until CR is sourced.
//
// Reticulum: no fixed target by design. Communities deliberately pick
// arbitrary settings offset from Meshtastic/MeshCore to avoid collision.
// Handled entirely by ENERGY_SWEEP (build-order phase 8) — never gets a
// ChannelParams entry.
//
// General Exploration: full 868-923MHz sweep, any SF/BW/CR. Also an
// ENERGY_SWEEP consumer, not a fixed channel.

// Superseded by Phase 4: main.cpp boots on CHANNEL_MESHTASTIC_LONGFAST_US
// (or its SD-config override, config.h) and radio_task.cpp now owns live
// switching between tables via channelParamsForProfile() above, per
// DESIGN.md §5 — kept as history, not current behaviour.
