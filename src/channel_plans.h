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
// RadioLib's begin()/setSyncWord() as a single byte; RadioLib handles the
// SX126x two-byte register mapping internally (which is what DESIGN.md §7's
// "0x2B vs. its two-byte register mapping" confusion was about — callers
// pass the one-byte value, same as Meshtastic's own firmware does).
//
// RadioLib's own default is RADIOLIB_SX126X_SYNC_WORD_PRIVATE == 0x12
// (verified in SX1262.h's begin() signature). Anything here that doesn't
// have a source-verified value should stay on that default explicitly
// rather than borrowing another protocol's number.
constexpr uint8_t SYNC_WORD_RADIOLIB_DEFAULT = 0x12;

// Meshtastic — VERIFIED against upstream firmware source, satisfying
// CLAUDE.md's house rule against hardcoding an unverified sync word:
// meshtastic/firmware `src/mesh/RadioLibInterface.h`, `const uint8_t
// syncWord = 0x2b;`. Its own comment resolves DESIGN.md §7's "sources
// disagree" note outright:
//   "For releases before 1.2 we used 0x12 (or for very old loads 0x14)
//    Note: do not use 0x34 - that is reserved for lorawan
//    We now use 0x2b ... We will be staying with this code for a long time."
// So 0x12 wasn't wrong so much as *stale* (pre-1.2 Meshtastic) — and it
// doubles as RadioLib's default, which is exactly how this firmware ended
// up silently listening on it through the 2026-08-23 bench tests. 0x34 as
// "LoRaWAN" also independently corroborates DESIGN.md §6's fingerprint table.
constexpr uint8_t SYNC_WORD_MESHTASTIC = 0x2B;

// MeshCore — VERIFIED 2026-08-23 against upstream source:
// meshcore-dev/MeshCore `src/helpers/radiolib/CustomSX1262.h`, which calls
//   begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, ...)
// i.e. MeshCore genuinely runs on RadioLib's stock private sync word, 0x12.
// Numerically identical to SYNC_WORD_RADIOLIB_DEFAULT, but kept as its own
// named constant deliberately: they mean different things (one is "MeshCore's
// verified value," the other is "we don't know yet"), and a future MeshCore
// release could move off it. Do NOT collapse these two into one constant.
//
// Note this is the *opposite* situation from Meshtastic: here the sniffer
// was accidentally already correct, so MeshCore RX was never broken by the
// 0x12 default the way Meshtastic RX was.
constexpr uint8_t SYNC_WORD_MESHCORE = 0x12;

// Preamble length is deliberately NOT in this struct yet. Finding
// (2026-08-23, both upstream sources): Meshtastic uses 16
// (`RadioInterface.h`: "8 is default, but we use longer to increase the
// amount of sleep time when receiving") and MeshCore also passes 16
// (`CustomSX1262.h`). This firmware leaves RadioLib's default of 8 — and
// that empirically does NOT block RX, confirmed by decoding live Meshtastic
// frames on hardware at 8. Left alone on purpose rather than "fixed": a
// continuous-RX receiver syncs on whatever preamble arrives, so this only
// starts to matter for the duty-cycled/CAD scanning in DESIGN.md §4 (phase
// 4+), where preamble length feeds detection timing directly. Revisit it
// there, with a bench test, instead of perturbing a working RX config now.
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
// caught by DISCOVERY_SWEEP (DESIGN.md §4, build-order phase 4).
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
