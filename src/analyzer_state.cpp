#include "analyzer_state.h"

#include <Arduino.h>
#include <freertos/semphr.h>

#include "radio_task.h"

namespace {

SemaphoreHandle_t analyzerMutex = nullptr;
CaptureHistory sharedCaptureHistory;
NodeRoster sharedNodeRoster;
WaterfallHistory sharedWaterfallHistory;

} // namespace

bool analyzerStateInit() {
    if (analyzerMutex == nullptr) {
        analyzerMutex = xSemaphoreCreateMutex();
    }
    return analyzerMutex != nullptr;
}

void analyzerNoteDetection(const Detection &det) {
    if (analyzerMutex == nullptr) return;
    const CaptureSummary summary = captureSummaryFromDetection(det);
    // detectionApplyMeshtasticHeader() (detection.h) sets node_id and the
    // hop_limit/hop_start/relay_node trio together, or zeroes all of them
    // together on a failed/absent parse — node_id != 0 is that same signal
    // reused here, not a new one. A 0 node_id also refuses the roster entry
    // outright (nodeRosterUpdate()'s own guard), which is exactly right for
    // MeshCore Detections: CLAUDE.md's house rule against parsing its
    // unverified header means node_id always stays 0 for them.
    const bool hasHopMetadata = det.node_id != 0;

    if (xSemaphoreTake(analyzerMutex, portMAX_DELAY) == pdTRUE) {
        captureHistoryPush(sharedCaptureHistory, summary);
        nodeRosterUpdate(sharedNodeRoster, det.node_id, det.rx_millis, det.rssi_dbm, det.snr_db,
                         det.profile, hasHopMetadata, det.hop_limit, det.hop_start);
        xSemaphoreGive(analyzerMutex);
    }
}

void analyzerNoteSweepComplete() {
    if (analyzerMutex == nullptr) return;
    const uint16_t binCount = radioEnergyBinCount();
    if (binCount == 0) return;

    // Occupancy-only for this slice, not a full per-bin RSSI capture:
    // Pass A's own internal per-bin stats (radio_task.cpp's EnergyBinStats)
    // live only for that bin's own dwell (docs/DESIGN.md's "never
    // accumulate raw samples" rule) and are gone well before the sweep
    // completes. The one thing that DOES survive to sweep-end is
    // radioEnergyPeakBinSetAtLastComplete()'s stable per-completed-sweep
    // snapshot — same peak bitmask drawSweepOccupancy() (ui_pages.cpp)
    // renders live for a single sweep, but a separate cross-core-safe copy
    // (radio_task.h's own comment on it): this function runs on Core 0,
    // and reading the live mask instead raced radio_task's repeat-mode
    // do-while resetting it for the next lap before this ever got polled
    // (found on real hardware 2026-09-03 -- every repeat-mode Waterfall
    // row came back quiet regardless of what Pass A found). Reusing this
    // snapshot gives a real, truthful hit/no-hit row per completed sweep
    // with zero new radio_task.cpp memory or plumbing, at the cost of
    // intensity resolution — revisit with a real per-bin capture if field
    // use calls for it.
    int16_t bins[WATERFALL_MAX_BINS];
    for (uint16_t b = 0; b < binCount && b < WATERFALL_MAX_BINS; b++) {
        bins[b] = radioEnergyPeakBinSetAtLastComplete(b) ? WATERFALL_RSSI_CEIL_DBM_X10
                                                          : WATERFALL_RSSI_FLOOR_DBM_X10;
    }

    // Packets actually decoded on the home channel during the between-lap
    // listen window (radio_task.cpp's performEnergySweepHomeListen(),
    // v1.0.3) — a strictly stronger fact about that frequency than a Pass-A
    // energy peak, since these were demodulated and CRC-checked, so the
    // Waterfall marks them distinctly rather than leaving the bin blank.
    // Both are the same at-completion snapshots as the peak mask above, for
    // the same cross-core reason. See waterfall.h's WaterfallRow comment for
    // why the count belongs to the window *before* this row's lap.
    const uint16_t captureBin = radioEnergyHomeBinAtLastComplete();
    const uint16_t captures = radioEnergyCapturesAtLastComplete();

    if (xSemaphoreTake(analyzerMutex, portMAX_DELAY) == pdTRUE) {
        waterfallHistoryPushRow(sharedWaterfallHistory, bins, binCount, millis(), captureBin,
                                captures > 255 ? 255 : (uint8_t)captures);
        xSemaphoreGive(analyzerMutex);
    }
}

bool analyzerCaptureHistorySnapshot(CaptureHistory &out, TickType_t timeout) {
    if (analyzerMutex == nullptr) return false;
    if (xSemaphoreTake(analyzerMutex, timeout) != pdTRUE) return false;
    out = sharedCaptureHistory;
    xSemaphoreGive(analyzerMutex);
    return true;
}

bool analyzerNodeRosterSnapshot(NodeRoster &out, TickType_t timeout) {
    if (analyzerMutex == nullptr) return false;
    if (xSemaphoreTake(analyzerMutex, timeout) != pdTRUE) return false;
    out = sharedNodeRoster;
    xSemaphoreGive(analyzerMutex);
    return true;
}

bool analyzerWaterfallRowSnapshot(uint8_t recency_index, WaterfallRow &out, TickType_t timeout) {
    if (analyzerMutex == nullptr) return false;
    if (xSemaphoreTake(analyzerMutex, timeout) != pdTRUE) return false;
    const WaterfallRow *row = waterfallHistoryRowAt(sharedWaterfallHistory, recency_index);
    const bool found = row != nullptr;
    if (found) out = *row;
    xSemaphoreGive(analyzerMutex);
    return found;
}

uint8_t analyzerWaterfallRowCount(TickType_t timeout) {
    if (analyzerMutex == nullptr) return 0;
    if (xSemaphoreTake(analyzerMutex, timeout) != pdTRUE) return 0;
    const uint8_t count = sharedWaterfallHistory.count;
    xSemaphoreGive(analyzerMutex);
    return count;
}
