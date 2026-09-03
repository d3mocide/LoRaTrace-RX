#pragma once
// LoRaTrace RX — Phase 10 Field Analyzer's shared derived-state layer.
//
// Owns the derived presentation structures ui_pages.cpp's Analyze views
// read (waterfall.h's WaterfallHistory, capture_history.h's CaptureHistory,
// node_roster.h's NodeRoster) — a thin cross-task module, not a FreeRTOS
// task of its own, same shape as memory_stats.h/profile_state.h.
// scope_trace.h's ScopeTrace is NOT stored or managed here: it stays owned
// by radio_task.cpp (radioScopeTraceSnapshot()) since only that task ever
// writes to it, the same reason activeChannel/activeProfile live there too.
// ANALYZER_STATIC_BYTES itself lives in analyzer_budget.h, not here, so it
// stays reachable (by session_log.h's writer and its host test) without
// pulling in FreeRTOS.h.
//
// logger_task.cpp (Core 0) is the only writer — it already has every real
// Detection/completed-Sweep result in hand right where it dequeues them.
// ui_task.cpp (also Core 0, but a different task, so still preemptible
// mid-access) only ever reads snapshots through the mutex-guarded
// accessors below — same "copy under a short mutex, render after releasing
// it" contract radioScopeTraceSnapshot() already established.

#include <freertos/FreeRTOS.h>

#include "analyzer_budget.h" // ANALYZER_STATIC_BYTES
#include "capture_history.h"
#include "detection.h"
#include "node_roster.h"
#include "waterfall.h"

// Must be called once before any other function here (main.cpp, alongside
// the other task/module init calls, before loggerTaskStart()). Returns
// false if its mutex couldn't be created.
bool analyzerStateInit();

// Called by logger_task.cpp right after dequeuing a real Detection —
// updates Recent Captures and the passive node roster from it. Safe to call
// whether or not the detection is ever durably logged: this is presentation
// state, not the mission log.
void analyzerNoteDetection(const Detection &det);

// Called by logger_task.cpp when it observes radioEnergySweepCount() (radio_task.h)
// advance — a Sweep just completed. Builds one Waterfall row from the
// completed sweep's peak-bin occupancy; see analyzer_state.cpp for why this
// slice is occupancy-only, not a full per-bin RSSI capture.
void analyzerNoteSweepComplete();

// Snapshot accessors — copy under a short mutex, safe to call from any task.
// Return false if the mutex couldn't be taken within `timeout`.
bool analyzerCaptureHistorySnapshot(CaptureHistory &out, TickType_t timeout = pdMS_TO_TICKS(250));
bool analyzerNodeRosterSnapshot(NodeRoster &out, TickType_t timeout = pdMS_TO_TICKS(250));

// Row-at-a-time, deliberately NOT a whole-WaterfallHistory snapshot: that
// struct is ~5.5KB (24 rows x ~232B), far too large to safely copy onto any
// task's stack as a local — ui_task's own stack is only 4096B total, and a
// first cut of this API that returned the whole struct did exactly that and
// crashed ui_task on real hardware (Waterfall page, 2026-09-03). One
// WaterfallRow (~232B) is a normal-sized local; recency_index 0 = most
// recently pushed row. Returns false if out of range or the mutex couldn't
// be taken within `timeout`.
bool analyzerWaterfallRowSnapshot(uint8_t recency_index, WaterfallRow &out,
                                  TickType_t timeout = pdMS_TO_TICKS(250));
// How many rows are actually available right now (<= WATERFALL_MAX_ROWS) —
// callers loop recency_index 0..count-1. Returns 0 if the mutex couldn't be
// taken within `timeout`, same as "nothing to show" from the caller's
// point of view.
uint8_t analyzerWaterfallRowCount(TickType_t timeout = pdMS_TO_TICKS(250));
