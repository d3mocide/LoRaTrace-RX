#pragma once
// LoRaTrace RX — Phase 9 ENERGY_SWEEP working stats + fixed observation
// record. Separate from both Detection and ScanObservation: an energy
// average/peak is not a CAD result any more than a CAD result is a packet
// (scan_observation.h's own reasoning, one level down).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "detection.h"  // missionProfileName()

// --- Pass A working aggregate (radio task, per bin, RAM only) ----------
//
// Streaming stats only — DESIGN.md §7.3/§8.1: never accumulate raw
// samples. rssi_peak_dbm_x10 defaults to this sentinel (not 0, a real
// plausible strong-signal reading) so the first real sample always wins
// the running max — same "fail toward an impossible value, not a
// plausible-looking zero" convention as ScanObservation's RADIO_ERROR
// default below.
constexpr int16_t ENERGY_BIN_NO_SAMPLE_PEAK = -32768;

// bit0: sample_count saturated at 255. bits1-7 reserved for a future
// Pass-B slice (e.g. "CAD attempted here").
constexpr uint8_t ENERGY_BIN_FLAG_SAMPLE_OVERFLOW = 0x01;

struct EnergyBinStats {
    int16_t rssi_avg_dbm_x10 = 0;
    int16_t rssi_peak_dbm_x10 = ENERGY_BIN_NO_SAMPLE_PEAK;
    uint8_t sample_count = 0;
    uint8_t occupied_count = 0; // samples that cleared the floor threshold
    uint8_t flags = 0;
};
// DESIGN.md §7.3: "roughly 8 bytes... less than 1.8KB for the current
// sweep" (224 bins x 8B = 1,792B). Load-bearing for that claim, not a
// loose ceiling.
static_assert(sizeof(EnergyBinStats) <= 8,
              "EnergyBinStats exceeds the Phase 9 ~8B/bin budget (DESIGN.md §7.3)");

// Incremental mean + running max. Integer arithmetic only, no float drift.
inline void energyBinStatsAddSample(EnergyBinStats &stats, int16_t rssi_dbm_x10) {
    if (stats.sample_count == 0) {
        stats.rssi_avg_dbm_x10 = rssi_dbm_x10;
    } else {
        stats.rssi_avg_dbm_x10 = (int16_t)(stats.rssi_avg_dbm_x10 +
            (int32_t)(rssi_dbm_x10 - stats.rssi_avg_dbm_x10) / (int32_t)(stats.sample_count + 1));
    }
    if (rssi_dbm_x10 > stats.rssi_peak_dbm_x10) stats.rssi_peak_dbm_x10 = rssi_dbm_x10;
    if (stats.sample_count < 255) {
        stats.sample_count++;
    } else {
        stats.flags |= ENERGY_BIN_FLAG_SAMPLE_OVERFLOW;
    }
}

// Separate from AddSample: occupancy depends on the rolling floor at
// sample time, which AddSample doesn't know about — callers pair this
// with energyExceedsFloor() below.
inline void energyBinStatsNoteOccupancy(EnergyBinStats &stats, bool occupied) {
    if (occupied && stats.occupied_count < 255) stats.occupied_count++;
}

// --- Rolling noise floor + peak decision --------------------------------
//
// Fixed-point EMA, tenths-of-dBm. Division (not a shift) avoids C++'s
// pre-C++20 implementation-defined negative right-shift entirely — this
// runs in the radio task's polling loop, not DIO1's ISR, so CLAUDE.md's
// ISR constraint doesn't apply. Divisor 8 => an ~8-sample time constant;
// DESIGN.md §7.3's own real quiet-band/injected-signal calibration sets
// the final number — this is a compiling, host-testable placeholder.
constexpr int16_t ENERGY_NOISE_FLOOR_EMA_DIVISOR = 8;

constexpr int16_t energyNoiseFloorUpdate(int16_t floor_dbm_x10, int16_t sample_dbm_x10) {
    return (int16_t)(floor_dbm_x10 +
        (int32_t)(sample_dbm_x10 - floor_dbm_x10) / ENERGY_NOISE_FLOOR_EMA_DIVISOR);
}
// NOTE for the radio_task.cpp integration slice: which samples feed this
// (every Pass-A sample vs. a below-median subset, so a strong peak doesn't
// drag its own floor upward) is an acquisition-sequencing decision left
// for that slice — this file only provides the arithmetic primitive.

// Placeholder margin pending §7.3's real calibration (see above).
constexpr int16_t ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10 = 100; // 10.0dB

constexpr bool energyExceedsFloor(int16_t value_dbm_x10, int16_t floor_dbm_x10,
                                   int16_t margin_dbm_x10 = ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10) {
    return value_dbm_x10 >= (int16_t)(floor_dbm_x10 + margin_dbm_x10);
}

// DESIGN.md §8.1: "don't dump every sweep point, only peaks." Uses the
// bin's *peak* (not average): a short burst inside a mostly-quiet dwell
// window must not be smoothed away by its own quieter samples — peak is
// always >= average, so this is strictly more sensitive than an
// average-based test.
inline bool energyBinIsPeak(const EnergyBinStats &stats, int16_t floor_dbm_x10,
                             int16_t margin_dbm_x10 = ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10) {
    if (stats.sample_count == 0) return false;
    return energyExceedsFloor(stats.rssi_peak_dbm_x10, floor_dbm_x10, margin_dbm_x10);
}

// float dBm (RadioLib's getRSSI()) -> tenths-of-dBm fixed point. Round
// half-away-from-zero without <math.h>. int16 range vastly exceeds any
// physically plausible SX1262 reading, so the clamp is defense-in-depth
// against a garbage float, not a real-world concern.
inline int16_t energyRssiDbmToFixed(float rssi_dbm) {
    float scaled = rssi_dbm * 10.0f;
    scaled = (scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f);
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32768.0f) scaled = -32768.0f;
    return (int16_t)scaled;
}

// --- Queued/logged record (Core1 -> Core0, energy.csv) ------------------

enum class EnergyObservationResult : uint8_t {
    ENERGY_PEAK = 0, // Pass A: threshold-filtered peak, no CAD attempted
    RADIO_ERROR,     // tune/RSSI-read failed while acquiring this bin
    // Pass-B CAD_FREE/CAD_DETECTED/CAD_TIMEOUT values are deliberately NOT
    // added yet (a later Phase 9 slice): SX1262 CAD-at-arbitrary-bin
    // behavior isn't verified. Appending new values later is non-breaking.
};

inline const char *energyObservationResultName(EnergyObservationResult result) {
    switch (result) {
        case EnergyObservationResult::ENERGY_PEAK: return "energy_peak";
        case EnergyObservationResult::RADIO_ERROR: return "radio_error";
        default: return "unknown";
    }
}

// Field order: DESIGN.md §5.2's Identity/Tuning/Result/Context groups map
// as: Identity = rx_millis, profile, bin_index (run id + GPS fix stay CSV
// format params, not struct fields — same as ScanObservation, keeping the
// radio task GPS-free); Tuning = freq_mhz, bin_step_khz,
// sf/bw_khz_x10/cr_denom/sync_word (0 = "no CAD attempted this record"
// sentinel — Pass B doesn't exist yet, same "0 = unknown" convention as
// Detection.node_id); Result = rssi_avg/peak, sample_count, result,
// packet_metadata_present (always false until a later slice adds Pass-B
// receive-on-hit, which will produce a separate Detection row exactly
// like Probe's own receive-on-hit does); Context = wifi_on (GPS again via
// CSV params); Result status = radio_status. 4-byte members grouped first
// so the struct packs without padding holes.
struct EnergyObservation {
    uint32_t rx_millis = 0;
    float freq_mhz = 0.0f;
    uint16_t bw_khz_x10 = 0;
    uint16_t bin_step_khz = 0;
    int16_t rssi_avg_dbm_x10 = 0;
    int16_t rssi_peak_dbm_x10 = 0;
    int16_t radio_status = 0;
    uint8_t profile = 0; // MissionProfile
    uint8_t bin_index = 0;
    uint8_t sf = 0;
    uint8_t cr_denom = 0;
    uint8_t sync_word = 0;
    uint8_t sample_count = 0;
    EnergyObservationResult result = EnergyObservationResult::RADIO_ERROR;
    bool packet_metadata_present = false;
    bool wifi_on = false;
};
// §5.2's own evaluation ceiling is 48B/record; ~28B actual leaves slack
// the same way ScanObservation's <=24 assert does against its own size.
static_assert(sizeof(EnergyObservation) <= 32,
              "EnergyObservation exceeds the Phase 9 slice-1 queue budget");

constexpr const char *ENERGY_CSV_HEADER =
    "timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,result,"
    "bin_index,bin_step_khz,freq_mhz,sf,bw_khz,cr_denom,sync_word,"
    "rssi_avg_dbm,rssi_peak_dbm,sample_count,packet_metadata_present,"
    "wifi_on,radio_status";

inline size_t energyObservationFormatCsv(const EnergyObservation &observation,
                                          char *out, size_t outSize,
                                          const char *timestamp_utc, bool has_fix,
                                          double lat, double lon, uint8_t fix_quality,
                                          uint16_t run) {
    if (out == nullptr || outSize == 0) return 0;

    char latbuf[16], lonbuf[16];
    if (has_fix) {
        snprintf(latbuf, sizeof(latbuf), "%.6f", lat);
        snprintf(lonbuf, sizeof(lonbuf), "%.6f", lon);
    } else {
        latbuf[0] = '\0';
        lonbuf[0] = '\0';
    }

    const int n = snprintf(
        out, outSize,
        "%s,%s,%s,%u,%u,%lu,%s,%s,%u,%u,%.3f,%u,%.1f,%u,0x%02x,%.1f,%.1f,%u,%u,%u,%d",
        timestamp_utc ? timestamp_utc : "", latbuf, lonbuf,
        (unsigned)fix_quality, (unsigned)run,
        (unsigned long)observation.rx_millis,
        missionProfileName(observation.profile),
        energyObservationResultName(observation.result),
        (unsigned)observation.bin_index, (unsigned)observation.bin_step_khz,
        (double)observation.freq_mhz, (unsigned)observation.sf,
        (double)observation.bw_khz_x10 / 10.0, (unsigned)observation.cr_denom,
        (unsigned)observation.sync_word,
        (double)observation.rssi_avg_dbm_x10 / 10.0,
        (double)observation.rssi_peak_dbm_x10 / 10.0,
        (unsigned)observation.sample_count,
        (unsigned)observation.packet_metadata_present,
        (unsigned)observation.wifi_on, (int)observation.radio_status);

    if (n < 0 || (size_t)n >= outSize) return 0;
    return (size_t)n;
}
