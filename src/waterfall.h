#pragma once
// LoRaTrace RX — Phase 10 Field Analyzer, Waterfall view storage.
//
// Pure presentation layer over real Phase 9 Sweep bins (docs/research/
// LoRaTrace-Phases-7-10-Design.md §8.2/§8.3) — it never talks to the radio.
// Storage is per real frequency bin (up to energy_plan.h's own
// ENERGY_BIN_RESERVED_COUNT), not pre-reduced to a display width: bin count
// varies with the Sweep Region setting (US ~84 bins vs Global 221 at the
// 250kHz step), so reducing to pixel columns is a render-time concern
// (waterfallAggregateRow() below), not a storage-time one.

#include <stddef.h>
#include <stdint.h>

#include "energy_plan.h"  // ENERGY_BIN_RESERVED_COUNT

constexpr uint8_t WATERFALL_MAX_ROWS = 24;
constexpr uint16_t WATERFALL_MAX_BINS = ENERGY_BIN_RESERVED_COUNT;

// §8.3's own 224 x 24 x 1B ceiling, checked directly rather than trusted.
static_assert((size_t)WATERFALL_MAX_ROWS * (size_t)WATERFALL_MAX_BINS == 5376,
              "waterfall bin storage must match the design doc's 224x24x1B ceiling "
              "(docs/research/LoRaTrace-Phases-7-10-Design.md §8.3)");

// Linear tenths-of-dBm -> byte quantization. Floor/ceiling are a generous
// envelope around any physically plausible SX1262 RSSI reading (not a
// bench-calibrated figure like energy_observation.h's noise-floor margin),
// so a value outside it clamps rather than wrapping. 255 is reserved as the
// "no real sample" sentinel, matching Detection/EnergyObservation's own
// "reserve one value for absence, don't reuse a plausible reading" pattern.
constexpr int16_t WATERFALL_RSSI_FLOOR_DBM_X10 = -1400; // -140.0dBm
constexpr int16_t WATERFALL_RSSI_CEIL_DBM_X10 = 0;      // 0.0dBm
constexpr uint8_t WATERFALL_NO_DATA = 0xFF;

inline uint8_t waterfallEncodeRssiByte(int16_t rssi_dbm_x10) {
    if (rssi_dbm_x10 <= WATERFALL_RSSI_FLOOR_DBM_X10) return 0;
    if (rssi_dbm_x10 >= WATERFALL_RSSI_CEIL_DBM_X10) return 254;
    const int32_t range = WATERFALL_RSSI_CEIL_DBM_X10 - WATERFALL_RSSI_FLOOR_DBM_X10;
    const int32_t offset = rssi_dbm_x10 - WATERFALL_RSSI_FLOOR_DBM_X10;
    return (uint8_t)((offset * 254) / range);
}

// Inverse of the above. Callers must check WATERFALL_NO_DATA separately —
// decoding it anyway would return a plausible-looking floor reading instead
// of "no sample," the same failure mode ENERGY_BIN_NO_SAMPLE_PEAK's comment
// (energy_observation.h) already calls out.
inline int16_t waterfallDecodeRssiByte(uint8_t value) {
    const int32_t range = WATERFALL_RSSI_CEIL_DBM_X10 - WATERFALL_RSSI_FLOOR_DBM_X10;
    return (int16_t)(WATERFALL_RSSI_FLOOR_DBM_X10 + ((int32_t)value * range) / 254);
}

// Deterministic bin -> plot-column mapping, endpoint-anchored both when
// downsampling (column_count < bin_count, the common case: 221 real bins
// onto a narrower physical panel) and upsampling: bin 0 always lands on
// column 0 and the last real bin always lands on the last column, so
// §8.6's "UI chrome never silently discards endpoint bins" holds regardless
// of the ratio between the two.
inline uint16_t waterfallColumnForBin(uint16_t bin_index, uint16_t bin_count, uint16_t column_count) {
    if (bin_count == 0 || column_count == 0) return 0;
    if (bin_index >= bin_count) bin_index = (uint16_t)(bin_count - 1);
    if (bin_count == 1 || column_count == 1) return 0;
    const uint32_t col = (uint32_t)bin_index * (uint32_t)(column_count - 1) / (uint32_t)(bin_count - 1);
    return (uint16_t)col;
}

// Max-aggregates every bin mapping to the same column, so a narrow real peak
// isn't smoothed away by quieter neighbors sharing its column — same
// reasoning as energy_observation.h's energyBinIsPeak() using bin peak, not
// average. Columns with no mapped bin (bin_count < column_count, an
// upsampling gap) stay WATERFALL_NO_DATA rather than a fabricated value —
// §8.6 "no fabricated vertical texture."
inline void waterfallAggregateRow(const int16_t *bin_peak_dbm_x10, uint16_t bin_count,
                                   uint8_t *out_columns, uint16_t column_count) {
    if (out_columns == nullptr || column_count == 0) return;
    for (uint16_t c = 0; c < column_count; c++) out_columns[c] = WATERFALL_NO_DATA;
    if (bin_peak_dbm_x10 == nullptr || bin_count == 0) return;
    for (uint16_t b = 0; b < bin_count; b++) {
        const uint16_t col = waterfallColumnForBin(b, bin_count, column_count);
        const uint8_t encoded = waterfallEncodeRssiByte(bin_peak_dbm_x10[b]);
        if (out_columns[col] == WATERFALL_NO_DATA || encoded > out_columns[col]) {
            out_columns[col] = encoded;
        }
    }
}

// One completed Sweep's worth of per-bin readings, stored at full bin
// resolution. bin_count can differ row to row (Region toggled between
// sweeps); bins beyond it stay WATERFALL_NO_DATA.
struct WaterfallRow {
    uint8_t bins[WATERFALL_MAX_BINS] = {};
    uint16_t bin_count = 0;
    uint32_t rx_millis = 0;
};

struct WaterfallHistory {
    WaterfallRow rows[WATERFALL_MAX_ROWS];
    uint8_t count = 0;      // rows populated so far, saturates at WATERFALL_MAX_ROWS
    uint8_t next_index = 0; // ring write cursor
};

inline void waterfallHistoryPushRow(WaterfallHistory &history, const int16_t *bin_peak_dbm_x10,
                                     uint16_t bin_count, uint32_t rx_millis) {
    if (bin_count > WATERFALL_MAX_BINS) bin_count = WATERFALL_MAX_BINS;
    WaterfallRow &row = history.rows[history.next_index];
    for (uint16_t b = 0; b < bin_count; b++) {
        row.bins[b] = (bin_peak_dbm_x10 != nullptr) ? waterfallEncodeRssiByte(bin_peak_dbm_x10[b])
                                                      : WATERFALL_NO_DATA;
    }
    for (uint16_t b = bin_count; b < WATERFALL_MAX_BINS; b++) row.bins[b] = WATERFALL_NO_DATA;
    row.bin_count = bin_count;
    row.rx_millis = rx_millis;
    history.next_index = (uint8_t)((history.next_index + 1) % WATERFALL_MAX_ROWS);
    if (history.count < WATERFALL_MAX_ROWS) history.count++;
}

// recency_index 0 = most recently pushed row (the top of the waterfall).
inline const WaterfallRow *waterfallHistoryRowAt(const WaterfallHistory &history, uint8_t recency_index) {
    if (recency_index >= history.count) return nullptr;
    const uint8_t idx =
        (uint8_t)((history.next_index + WATERFALL_MAX_ROWS - 1 - recency_index) % WATERFALL_MAX_ROWS);
    return &history.rows[idx];
}
