#pragma once
// LoRaTrace RX — Phase 9 Sweep Pass B: a small, sourced set of SF/BW
// combinations to CAD-test at each Pass-A peak bin. See
// research/phase9-sweep-pass-b-design.md for the full design.
//
// Every (sf, bw_khz) pair below already appears as a real upstream
// Meshtastic/MeshCore default somewhere in channel_plans.h/discovery_plan.h
// — this table exists only to deduplicate them into one small, bounded set
// Pass B iterates per peak bin, not to invent new modem parameters.

#include <stddef.h>
#include <stdint.h>

#include "channel_plans.h"

struct PassBModemParams {
    uint8_t sf;
    float bw_khz;
    // Coding rate doesn't affect CAD (it only shapes payload decoding), so
    // one placeholder value is shared across every row. Sync word is a
    // genuinely open bench question, not a settled non-issue: whether CAD
    // detection is sync-word-sensitive on this exact SX1262/RadioLib combo
    // (Semtech's LoRa CAD notes describe sync-word-dependent preamble
    // symbols on some parts) is exactly the kind of thing
    // research/phase9-sweep-pass-b-design.md's standing hardware unknown
    // needs a real bench matrix to answer — RadioLib's own stock default
    // (SYNC_WORD_MESHCORE's value, 0x12) is used here as a first,
    // deliberately non-proprietary placeholder, not a verified choice.
    uint8_t cr_denom;
    uint8_t sync_word;
};

constexpr uint8_t PASS_B_CR_DENOM_PLACEHOLDER = 5;      // 4/5, matches most of the sourced list below
constexpr uint8_t PASS_B_SYNC_WORD_PLACEHOLDER = SYNC_WORD_MESHCORE; // RadioLib's own stock default (0x12)

constexpr PassBModemParams PASS_B_SF_BW_CANDIDATES[] = {
    {7, 62.5f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER},  // MeshCore US Narrow (current default)
    {7, 250.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic ShortFast
    {8, 125.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // MeshOregon (operator local physical tuple)
    {8, 250.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic ShortSlow
    {9, 250.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic MediumFast
    {10, 250.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic MediumSlow / MeshCore upstream default
    {11, 125.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic LongModerate
    {11, 250.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic LongFast
    {11, 500.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic LongTurbo
    {12, 125.0f, PASS_B_CR_DENOM_PLACEHOLDER, PASS_B_SYNC_WORD_PLACEHOLDER}, // Meshtastic LongSlow
};
constexpr uint8_t PASS_B_SF_BW_CANDIDATE_COUNT =
    (uint8_t)(sizeof(PASS_B_SF_BW_CANDIDATES) / sizeof(PASS_B_SF_BW_CANDIDATES[0]));

// research/phase9-sweep-pass-b-design.md's starting bound. Originally "the
// 8 strongest peaks, decided after the full sweep" — revised 2026-08-28
// (operator observation: a deferred pass can arrive after a brief
// transmitter has already gone quiet) to "the first 8 peaks encountered,
// checked immediately upon discovery." Same worst-case cost either way: at
// PASS_B_SF_BW_CANDIDATE_COUNT (10) combos x Probe's existing 300ms CAD
// timeout, ~24s worst-case total across one Sweep run — revisable once the
// hardware bench matrix exists.
constexpr uint16_t PASS_B_MAX_PEAKS_PER_SWEEP = 8;

// Per-combo evidentiary weight, descriptive only -- never gates whether a
// CAD/Detection row gets logged. Sourced from
// scripts/phase9_pass_b_cad_bench.py's three independent 400-cycle bench
// runs (2026-08-29, research/phase9-sweep-pass-b-design.md's "Shielded-box
// quiet control" section: open-room, both-radios-shielded, Cardputer-only-
// shielded). Pooled 60 quiet + 60 pulse samples per combo across all
// three physical setups; only these two combos replicated consistently
// enough to trust either way -- the other eight varied too much run-to-run
// at n=20/condition to classify yet, so they stay UNVERIFIED.
enum class PassBConfidence : uint8_t {
    UNVERIFIED = 0, // not yet reproduced across independent runs
    NOISY = 1,      // reliably catches the real signal, but also fires often on nothing
    STRONG = 2,     // low false-positive rate AND reliable real-signal detection, reproduced 3x
};

inline const char *passBConfidenceName(PassBConfidence confidence) {
    switch (confidence) {
        case PassBConfidence::STRONG: return "high";
        case PassBConfidence::NOISY: return "noisy";
        case PassBConfidence::UNVERIFIED:
        default: return "unverified";
    }
}

// bw_khz_x10 matches EnergyObservation's own fixed-point field (bw_khz*10,
// rounded) so this compares integers, not floats.
inline PassBConfidence passBConfidenceFor(uint8_t sf, uint16_t bw_khz_x10) {
    if (sf == 8 && bw_khz_x10 == 1250) return PassBConfidence::STRONG;  // SF8/BW125: 2/60 quiet FP, 20/20 pulse every run
    if (sf == 11 && bw_khz_x10 == 5000) return PassBConfidence::NOISY; // SF11/BW500: 22/60 quiet FP, 19-20/20 pulse every run
    return PassBConfidence::UNVERIFIED;
}
