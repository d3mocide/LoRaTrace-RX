#pragma once
// LoRaTrace RX — Phase 10 Field Analyzer, Scope view storage.
//
// docs/research/LoRaTrace-Phases-7-10-Design.md §8.2: "x = time; y = RSSI
// sampled by bounded SCOPE_ACQUIRE at one fixed tuned frequency... do not
// call this a spectrum." Pure ring buffer + encoding; the radio-owned
// SCOPE_ACQUIRE state that actually feeds it is a separate, later slice
// (radio_task.cpp) — this header only holds and formats what it's given.

#include <stddef.h>
#include <stdint.h>

// §8.3: "at most 240 signed 8-bit samples plus timestamp/scale metadata."
constexpr uint16_t SCOPE_MAX_SAMPLES = 240;

// Whole-dBm int8 storage: real SX1262 RSSI readings in this project's own
// measured environments (docs/STATUS.md's Phase 9 bench data) sit well
// inside [-128, 127], and tenths-of-dBm precision isn't visible at scope
// plot resolution anyway (unlike energy_observation.h's fixed-point
// tenths, which feed a calibrated dB margin comparison). Clamped, not
// wrapped, so an out-of-range float can't alias to a plausible reading.
inline int8_t scopeEncodeRssiDbm(float rssi_dbm) {
    float rounded = (rssi_dbm >= 0.0f) ? (rssi_dbm + 0.5f) : (rssi_dbm - 0.5f);
    if (rounded > 127.0f) rounded = 127.0f;
    if (rounded < -128.0f) rounded = -128.0f;
    return (int8_t)rounded;
}

inline float scopeDecodeRssiDbm(int8_t value) { return (float)value; }

struct ScopeTrace {
    int8_t samples[SCOPE_MAX_SAMPLES] = {};
    uint32_t start_millis = 0;       // rx_millis of the acquisition this trace belongs to
    uint16_t sample_interval_ms = 0; // nominal spacing between samples
    float tuned_freq_mhz = 0.0f;     // the one frequency this trace samples (§8.2)
    uint16_t count = 0;              // samples valid so far, saturates at SCOPE_MAX_SAMPLES
    uint16_t next_index = 0;         // ring write cursor
};

// Starts a fresh trace for a new SCOPE_ACQUIRE run. Old samples are not
// zeroed (count/next_index reset makes them unreachable) — cheap, and no
// stale sample is ever readable through the public accessor below.
inline void scopeTraceReset(ScopeTrace &trace, float tuned_freq_mhz, uint16_t sample_interval_ms,
                             uint32_t start_millis) {
    trace.tuned_freq_mhz = tuned_freq_mhz;
    trace.sample_interval_ms = sample_interval_ms;
    trace.start_millis = start_millis;
    trace.count = 0;
    trace.next_index = 0;
}

inline void scopeTracePush(ScopeTrace &trace, float rssi_dbm) {
    trace.samples[trace.next_index] = scopeEncodeRssiDbm(rssi_dbm);
    trace.next_index = (uint16_t)((trace.next_index + 1) % SCOPE_MAX_SAMPLES);
    if (trace.count < SCOPE_MAX_SAMPLES) trace.count++;
}

// recency_index 0 = most recently pushed sample.
inline bool scopeTraceSampleAt(const ScopeTrace &trace, uint16_t recency_index, int8_t &out_value) {
    if (recency_index >= trace.count) return false;
    const uint16_t idx =
        (uint16_t)((trace.next_index + SCOPE_MAX_SAMPLES - 1 - recency_index) % SCOPE_MAX_SAMPLES);
    out_value = trace.samples[idx];
    return true;
}
