#pragma once
// LoRaTrace RX — Phase 10 Field Analyzer, Recent Captures view storage.
//
// docs/research/LoRaTrace-Phases-7-10-Design.md §8.2: "time, profile,
// frequency, SF/BW/CR, RSSI/SNR, length, safe cleartext header IDs. No
// payload hex, plaintext, or key handling." A fixed-size summary of a real
// Detection, not a second copy of it — deliberately drops raw_packet so
// nothing here can leak into a view that isn't supposed to carry payload
// bytes.

#include <stdint.h>

#include "detection.h"

// §8.3.
constexpr uint8_t CAPTURE_HISTORY_MAX_ENTRIES = 8;

struct CaptureSummary {
    uint32_t rx_millis = 0;
    uint32_t node_id = 0; // Detection's own safe cleartext header id; 0 = unknown
    float freq_mhz = 0.0f;
    float rssi_dbm = 0.0f;
    float snr_db = 0.0f;
    uint16_t raw_len = 0;
    uint16_t bw_khz_x10 = 0;
    uint8_t sf = 0;
    uint8_t cr_denom = 0;
    uint8_t profile = 0; // MissionProfile
    bool off_grid = false;
};

inline CaptureSummary captureSummaryFromDetection(const Detection &det) {
    CaptureSummary summary;
    summary.rx_millis = det.rx_millis;
    summary.node_id = det.node_id;
    summary.freq_mhz = det.freq_mhz;
    summary.rssi_dbm = det.rssi_dbm;
    summary.snr_db = det.snr_db;
    summary.raw_len = det.raw_len;
    summary.bw_khz_x10 = det.bw_khz_x10;
    summary.sf = det.sf;
    summary.cr_denom = det.cr_denom;
    summary.profile = det.profile;
    summary.off_grid = det.off_grid;
    return summary;
}

struct CaptureHistory {
    CaptureSummary entries[CAPTURE_HISTORY_MAX_ENTRIES];
    uint8_t count = 0;      // entries populated so far, saturates at CAPTURE_HISTORY_MAX_ENTRIES
    uint8_t next_index = 0; // ring write cursor
};

inline void captureHistoryPush(CaptureHistory &history, const CaptureSummary &summary) {
    history.entries[history.next_index] = summary;
    history.next_index = (uint8_t)((history.next_index + 1) % CAPTURE_HISTORY_MAX_ENTRIES);
    if (history.count < CAPTURE_HISTORY_MAX_ENTRIES) history.count++;
}

// recency_index 0 = most recently captured.
inline bool captureHistoryEntryAt(const CaptureHistory &history, uint8_t recency_index,
                                   CaptureSummary &out) {
    if (recency_index >= history.count) return false;
    const uint8_t idx = (uint8_t)((history.next_index + CAPTURE_HISTORY_MAX_ENTRIES - 1 -
                                    recency_index) % CAPTURE_HISTORY_MAX_ENTRIES);
    out = history.entries[idx];
    return true;
}
