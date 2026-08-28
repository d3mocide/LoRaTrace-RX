#pragma once
// LoRaTrace RX — fixed CAD observation record for Phase 8 Probe.
//
// This is intentionally separate from Detection: a CAD result is not a
// packet, so it must not inflate RX counts or masquerade as a decoded row.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "detection.h"

enum class ScanObservationResult : uint8_t {
    CAD_FREE = 0,
    CAD_DETECTED,
    CAD_TIMEOUT,
    RADIO_ERROR,
};

struct ScanObservation {
    uint32_t rx_millis = 0;
    float freq_mhz = 0.0f;
    int16_t radio_status = 0;
    uint16_t bw_khz_x10 = 0;
    uint8_t sf = 0;
    uint8_t cr_denom = 0;
    uint8_t sync_word = 0;
    uint8_t profile = 0; // MissionProfile
    uint8_t candidate_index = 0;
    ScanObservationResult result = ScanObservationResult::RADIO_ERROR;
};

static_assert(sizeof(ScanObservation) <= 24,
              "ScanObservation exceeds the fixed Phase 8 queue budget");

inline const char *scanObservationResultName(ScanObservationResult result) {
    switch (result) {
        case ScanObservationResult::CAD_FREE: return "cad_free";
        case ScanObservationResult::CAD_DETECTED: return "cad_detected";
        case ScanObservationResult::CAD_TIMEOUT: return "cad_timeout";
        case ScanObservationResult::RADIO_ERROR: return "radio_error";
        default: return "unknown";
    }
}

// One durable row per CAD candidate. GPS coordinates are stamped by the
// logger task just like Detection rows; the radio task remains GPS/SD-free.
constexpr const char *SCAN_CSV_HEADER =
    "timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,result,"
    "freq_mhz,sf,bw_khz,cr_denom,sync_word,candidate_index,radio_status";

inline size_t scanObservationFormatCsv(const ScanObservation &observation,
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
        "%s,%s,%s,%u,%u,%lu,%s,%s,%.3f,%u,%.1f,%u,0x%02x,%u,%d",
        timestamp_utc ? timestamp_utc : "", latbuf, lonbuf,
        (unsigned)fix_quality, (unsigned)run,
        (unsigned long)observation.rx_millis,
        missionProfileName(observation.profile),
        scanObservationResultName(observation.result),
        (double)observation.freq_mhz, (unsigned)observation.sf,
        (double)observation.bw_khz_x10 / 10.0,
        (unsigned)observation.cr_denom, (unsigned)observation.sync_word,
        (unsigned)observation.candidate_index, (int)observation.radio_status);

    if (n < 0 || (size_t)n >= outSize) return 0;
    return (size_t)n;
}
