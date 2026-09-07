#pragma once
// LoRaTrace RX — Cell band-bin math. Pure formula, mirrors
// energy_plan.h's shape exactly, scoped to a different, much narrower band.
//
// Cell is NOT a mission profile and NOT a protocol decode: the SX1262
// only demodulates FSK/GFSK/MSK/LoRa/OOK, so it cannot decode GSM/CDMA/LTE.
// This is a bounded RSSI-vs-frequency presence sweep — "is there a strong
// carrier near this cell channel, and how strong" — reusing the same
// retune-and-sample mechanism ENERGY_SWEEP's Pass A already uses, but as an
// isolated engine (radio_task.cpp's performCellSweep(), separate from
// performEnergySweep()) so it never touches Phase 9's hardware-calibrated
// noise-floor/peak logic (docs/history/PROGRESS.md's 35.0dB margin was
// measured for a LoRa/RF-quiet bench environment, not continuous
// cellular-strength carriers — reusing that number here would be a guess
// dressed up as a calibration).

#include <stdint.h>

// North American Cellular Radiotelephone Service downlink (FCC Part 22;
// 3GPP Band 5 downlink) — the band the operator observed hits in near cell
// towers. Sits entirely inside the Cap LoRa-1262's tuned 868-923MHz front
// end (docs/DESIGN.md §1), so unlike General Exploration's 923-928MHz top
// end, there's no rolloff caveat here.
constexpr float CELL_SWEEP_BAND_LO_MHZ = 869.0f;
constexpr float CELL_SWEEP_BAND_HI_MHZ = 894.0f;
// Wait after startReceive() before the first RSSI sample of a bin.
//
// Measured 2026-09-07 on hardware (audit A29), two interleaved reps per
// value, 101 bins each:
//
//   settle  valid bins  median      settle  valid bins  median
//        0    33.5/101  -117.0 dBm       3    92.5/101   -98.0 dBm
//        1    44.0/101  -111.8 dBm       5   101.0/101   -91.3 dBm
//        2    83.5/101  -101.4 dBm      10   101.0/101   -90.8 dBm
//
// At 0 the receiver is sampled before it has settled, and two thirds of bins
// return a floor value that belongs to no frequency in particular — which is
// what produced the shifting 0.75MHz comb A29 describes. 5ms is the knee: it
// is the smallest value where every bin reads, and 10ms buys nothing but ~1s
// of lap time. Costs ~0.5s across a 101-bin lap.
//
// The same 5ms was found independently on 2026-09-04 while investigating the
// light-retune port, then lost when that port was reverted (see the revert
// note in radio_task.cpp's performCellSweep).
constexpr uint16_t CELL_SETTLE_MS = 5;

constexpr uint32_t CELL_SWEEP_BAND_LO_KHZ = 869000;
constexpr uint32_t CELL_SWEEP_BAND_HI_KHZ = 894000;

// Same 250kHz default as ENERGY_SWEEP (research/...Phases-7-10-Design.md
// §7.3) — no reason to pick a different granularity for a similarly-shaped
// sweep. Fixed, not an EnergyBinStep alias: this band's own constant, kept
// independent of energy_plan.h's enum so the two sweeps' step choices can
// diverge later without one file's edit touching the other.
constexpr uint16_t CELL_SWEEP_STEP_KHZ = 250;

// (894000-869000)/250 + 1 = 101 bins — small enough that logging every bin
// (not peak-filtered, see file header) is a modest CSV, not a RAM concern.
constexpr uint16_t CELL_BIN_RESERVED_COUNT = 128;

constexpr uint16_t cellBinCount() {
    return (uint16_t)((CELL_SWEEP_BAND_HI_KHZ - CELL_SWEEP_BAND_LO_KHZ) / CELL_SWEEP_STEP_KHZ) + 1;
}

constexpr float cellBinFrequencyMhz(uint16_t bin_index) {
    return CELL_SWEEP_BAND_LO_MHZ + (float)bin_index * (float)CELL_SWEEP_STEP_KHZ / 1000.0f;
}

// Nearest bin to freq_mhz, clamped into [0, count-1] — same out-of-range
// handling as energyBinIndexForFrequencyMhz(), for the same reason (a
// caller handing in a slightly-off frequency gets the closest real bin, not
// undefined behavior).
inline uint16_t cellBinIndexForFrequencyMhz(float freq_mhz) {
    const uint16_t count = cellBinCount();
    if (freq_mhz <= CELL_SWEEP_BAND_LO_MHZ) return 0;
    if (freq_mhz >= CELL_SWEEP_BAND_HI_MHZ) return (uint16_t)(count - 1);
    const float offset_khz = (freq_mhz - CELL_SWEEP_BAND_LO_MHZ) * 1000.0f;
    uint16_t idx = (uint16_t)(offset_khz / (float)CELL_SWEEP_STEP_KHZ + 0.5f);
    if (idx >= count) idx = (uint16_t)(count - 1);
    return idx;
}

static_assert(cellBinCount() == 101, "869-894MHz at 250kHz must be 101 bins");
static_assert(cellBinCount() <= CELL_BIN_RESERVED_COUNT,
              "cell sweep bin count exceeds the reserved ceiling");

// North American Cellular A/B channel blocks (47 CFR § 22.905 "Channels
// for cellular service", eCFR Title 47 Part 22 Subpart H) — the FCC's own
// downlink sub-band split within 869-894MHz. NOT a carrier-specific
// table: actual current licensee varies by market and has changed hands
// repeatedly via spectrum trading since these blocks were first assigned,
// so this labels the regulatory block only (CLAUDE.md's house rule
// against hardcoding unverified/unstable RF facts) — never a carrier
// name.
struct CellBandBlock {
    char label;
    float lo_mhz;
    float hi_mhz;
};

constexpr CellBandBlock CELL_BAND_BLOCKS[] = {
    {'A', 869.0f, 880.0f},
    {'B', 880.0f, 890.0f},
    {'A', 890.0f, 891.5f},
    {'B', 891.5f, 894.0f},
};
constexpr uint8_t CELL_BAND_BLOCK_COUNT = 4;
