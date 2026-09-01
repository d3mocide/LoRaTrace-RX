#pragma once
// LoRaTrace RX — Cell queued/logged record (cell.csv).
//
// Deliberately its own type, not a repurposed EnergyObservation: a cell-band
// RSSI reading is not an ENERGY_SWEEP peak (it isn't threshold-filtered, and
// its sf/bw/cr/sync fields would describe the SX1262's own LoRa modem
// config, not anything about the cellular carrier being measured) any more
// than a CAD result is a packet (scan_observation.h's own reasoning, one
// level down). Every bin gets a row — see cell_plan.h's file header for why
// this doesn't reuse energy_observation.h's calibrated peak-detection
// threshold.
//
// Reuses EnergyBinStats/energyBinStatsAddSample()/energyRssiDbmToFixed()
// from energy_observation.h: that streaming-mean/peak arithmetic and the
// float-dBm-to-fixed-point conversion are generic RSSI statistics, not
// anything LoRa- or Phase-9-threshold-specific, so duplicating them here
// would just be drift risk for no reason.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "detection.h"          // missionProfileName()
#include "energy_observation.h" // EnergyBinStats, energyBinStatsAddSample(), energyRssiDbmToFixed()

enum class CellObservationResult : uint8_t {
    MEASURED = 0, // RSSI sampled successfully, whatever the reading
    RADIO_ERROR,  // retune or RSSI read failed while acquiring this bin
};

inline const char *cellObservationResultName(CellObservationResult result) {
    switch (result) {
        case CellObservationResult::MEASURED: return "measured";
        case CellObservationResult::RADIO_ERROR: return "radio_error";
        default: return "unknown";
    }
}

// Field order follows energy_observation.h's Identity/Tuning/Result grouping
// (run id + GPS fix stay CSV format params, not struct fields, keeping the
// radio task GPS-free): Identity = rx_millis, profile, bin_index; Tuning =
// freq_mhz, rx_bw_khz (the SX1262's configured receive bandwidth during this
// reading — a measurement condition, not a claim about the cellular
// carrier's own bandwidth); Result = rssi_avg/peak, sample_count, result,
// radio_status.
struct CellObservation {
    uint32_t rx_millis = 0;
    float freq_mhz = 0.0f;
    uint16_t rx_bw_khz_x10 = 0;
    int16_t rssi_avg_dbm_x10 = 0;
    int16_t rssi_peak_dbm_x10 = 0;
    int16_t radio_status = 0;
    uint8_t profile = 0; // MissionProfile of whatever HOME_LISTEN profile was active
    uint8_t bin_index = 0;
    uint8_t sample_count = 0;
    CellObservationResult result = CellObservationResult::RADIO_ERROR;
};
static_assert(sizeof(CellObservation) <= 24,
              "CellObservation exceeds the fixed Cell queue budget");

constexpr const char *CELL_CSV_HEADER =
    "timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,result,"
    "bin_index,freq_mhz,rx_bw_khz,rssi_avg_dbm,rssi_peak_dbm,sample_count,radio_status";

inline size_t cellObservationFormatCsv(const CellObservation &observation, char *out,
                                       size_t outSize, const char *timestamp_utc, bool has_fix,
                                       double lat, double lon, uint8_t fix_quality, uint16_t run) {
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
        "%s,%s,%s,%u,%u,%lu,%s,%s,%u,%.3f,%.1f,%.1f,%.1f,%u,%d",
        timestamp_utc ? timestamp_utc : "", latbuf, lonbuf,
        (unsigned)fix_quality, (unsigned)run,
        (unsigned long)observation.rx_millis,
        missionProfileName(observation.profile),
        cellObservationResultName(observation.result),
        (unsigned)observation.bin_index, (double)observation.freq_mhz,
        (double)observation.rx_bw_khz_x10 / 10.0,
        (double)observation.rssi_avg_dbm_x10 / 10.0,
        (double)observation.rssi_peak_dbm_x10 / 10.0,
        (unsigned)observation.sample_count, (int)observation.radio_status);

    if (n < 0 || (size_t)n >= outSize) return 0;
    return (size_t)n;
}
