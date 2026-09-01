#pragma once

#include <stdint.h>

// Deterministic fault hooks used only by the dedicated cardputer-adv-bench
// build. Production firmware accepts no operation through this API.

enum class BenchFaultPoint : unsigned char {
    BEFORE_RETUNE,
    AFTER_RETUNE,
    CAD_WAIT,
    RX_WAIT,
    HOME_RESTORE_BEFORE,
    HOME_RESTORE_AFTER,
};

enum class BenchFaultAction : unsigned char {
    CANCEL,
    FAIL,
};

// Arm one one-shot hook with an argument such as "CAD_WAIT:FAIL". "CLEAR"
// disarms a pending hook. The command is deliberately bounded to named
// radio-task boundaries; it cannot issue arbitrary RadioLib calls.
bool benchFaultConfigure(const char *argument);

// Consumes the matching one-shot hook, if any. Called only at the boundaries
// named above by the radio task.
bool benchFaultTake(BenchFaultPoint point, BenchFaultAction &action);

// Bench-image-only CAD selector for the Phase 8 rate matrix. Production
// firmware always returns the source-backed two-symbol default and rejects
// changes, so serial control cannot retune its receiver behavior.
bool benchCadSymbolsConfigure(const char *argument);
unsigned char benchCadSymbols();

// Bench-image-only Sweep noise-floor margin override, for the Phase 9
// margin-calibration matrix (energy_observation.h's
// ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10 is an explicit placeholder
// pending exactly this). Argument is tenths of dB (e.g. "150" = 15.0dB).
// Production firmware always returns the same placeholder default and
// rejects changes, same production/bench split as the CAD selector above.
bool benchSweepMarginConfigure(const char *argument);
int16_t benchSweepMarginDbmX10();

// Bench-image-only gate for triggering one Pass B CAD attempt on demand
// (research/phase9-sweep-pass-b-design.md's false-positive-vs-SF bench
// matrix): production Pass B only ever runs at a real Pass-A peak, so this
// is the only way to exercise a specific PASS_B_SF_BW_CANDIDATES entry
// under a controlled quiet/pulse condition. Same production/bench split as
// the two selectors above -- production radio_task.cpp calls this to
// decide whether to accept the request at all, not just what value to use.
bool benchPassBCadTriggerAllowed();

// Bench-image-only per-bin noise-floor capture for the 923MHz-edge
// front-end rolloff characterization (docs/ROADMAP.md's Phase 9 blocking
// unknown, docs/STATUS.md's "still open" list). Production Pass A already
// computes each bin's average RSSI (energy_observation.h's EnergyBinStats)
// but discards it once the peak/no-peak decision is made -- energy.csv
// only ever persists threshold-filtered peaks, never the raw floor. This
// keeps the most recent sweep's full per-bin average so a bench harness
// can read it back one bin at a time after the sweep completes. Production
// firmware records/resets nothing and every query fails, same
// production/bench split as the rest of this file.
void benchSweepFloorReset();
void benchSweepFloorRecord(uint16_t bin, int16_t rssi_avg_dbm_x10);
bool benchSweepFloorQuery(uint16_t bin, int16_t &rssi_avg_dbm_x10);

// Bench-image-only gate for parking the radio at an arbitrary frequency and
// sampling RSSI continuously for a bounded window (radio_task.cpp's
// performBenchRssiWindow()) -- the 923MHz-edge injected-carrier
// characterization: a full Sweep's per-bin dwell (tens of ms) is too short
// to reliably catch an independently-timed transmitter's burst, so this
// holds still long enough to. Same production/bench split as the rest of
// this file.
bool benchRssiWindowTriggerAllowed();
