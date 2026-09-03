#pragma once
// LoRaTrace RX — Field Analyzer's static-storage budget.
//
// Split out of analyzer_state.h so this one constant stays reachable
// without pulling in FreeRTOS.h: analyzer_state.h's mutex-guarded
// accessors need it, but session_log.h (and this file's own host test)
// are kept pure (no Arduino, no FreeRTOS) so `pio test -e native` can
// build them — same reasoning as session_log.h's own header comment.

#include "capture_history.h"
#include "node_roster.h"
#include "scope_trace.h"
#include "waterfall.h"

// Total static bytes Field Analyzer's own fixed storage commits — the
// three structures analyzer_state.h owns plus ScopeTrace (owned by
// radio_task.cpp, not analyzer_state.h, but counted here since this is
// meant to be Phase 10's one number: docs/research/
// LoRaTrace-Phases-7-10-Design.md §9's "analyzer_static_bytes reported
// once per build/run"). A compile-time constant, not a runtime
// measurement — every one of these is a fixed-size global, not a dynamic
// allocation, so sizeof() is already authoritative.
constexpr uint32_t ANALYZER_STATIC_BYTES =
    (uint32_t)(sizeof(WaterfallHistory) + sizeof(CaptureHistory) + sizeof(NodeRoster) + sizeof(ScopeTrace));
