#pragma once

#include <stdint.h>

#include "energy_observation.h"

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
// ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10 is the resulting calibrated
// default). Argument is tenths of dB (e.g. "150" = 15.0dB). Production
// firmware rejects changes here (benchSweepMarginConfigure() always fails)
// — the operator-facing equivalent is System > Tuning > Margin
// (radio_task.h's radioSetEnergySweepMarginDbmX10()). benchSweepMarginDbmX10()
// resolves to that operator setting on production firmware and to this
// bench override on the dedicated cardputer-adv-bench image, so
// radio_task.cpp's Pass-A call sites don't need to know which build they're
// in — see its own two branches for the split.
bool benchSweepMarginConfigure(const char *argument);
int16_t benchSweepMarginDbmX10();

// Bench-image-only override for performEnergySweep()'s per-bin retune
// strategy. Argument is "FULL" (a complete radio.begin() at every bin, the
// pre-v1.0.2 behaviour) or "LIGHT" (standby/setFrequency/startReceive, what
// production ships).
//
// Exists to test M6 (docs/research/2026-09-04-project-audit.md): porting the
// light retune to Cell made it miss a real -73dBm carrier entirely while
// still reporting plausible noise, which looks like RSSI/AGC settling time
// that begin()'s overhead had been providing. Whether Sweep has the same
// under-read is unknown, and a *runtime* switch is what makes that
// answerable honestly -- both arms then run on one firmware image in one
// session, instead of comparing two separate builds across two flashes and
// two boots. Enough measurements this session were confounded by exactly
// that kind of difference.
//
// Production always reports LIGHT and rejects changes, same split as the
// margin/CAD selectors above.
bool benchSweepRetuneConfigure(const char *argument);
bool benchSweepRetuneFullEveryBin();

// Bench-image-only override for the light retune's settling delay, in ms
// (0-50). Production always returns ENERGY_SWEEP_SETTLE_DEFAULT_MS.
//
// M6 measured the light retune reporting a real carrier 11.1dB weaker than
// a full begin() does, while the noise floor differed by only 2.4dB -- an
// under-read that grows with signal strength, which is what an unsettled
// AGC looks like. A runtime knob is what makes the fix answerable: the
// right settle duration is an empirical question, and reflashing per
// candidate value would put a build boundary between every datapoint.
bool benchSweepSettleConfigure(const char *argument);
uint16_t benchSweepSettleMs();

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

// Bench-image-only readback of the most recent BENCH_PASS_B_CAD attempt's
// raw result (research/phase9-sweep-pass-b-design.md's standing
// CAD-at-arbitrary-bin ground-truth question). Production Pass B logs
// every attempt's CAD_FREE/CAD_DETECTED/CAD_TIMEOUT result to energy.csv,
// but that means pulling the SD card to see it -- this exposes the same
// result over Serial Control instead, so a host script (e.g. bench/rtl-sdr's
// sync_cad_capture.py) can correlate one specific attempt against what an
// independent receiver saw on the air at that exact moment. No RSSI value
// here -- CAD reports free/detected/timeout, not an amplitude, so
// EnergyObservation's own rssi_peak_dbm_x10 is always 0 for a Pass B row
// too. Recorded only for bench-triggered attempts (radio_task.cpp checks
// benchPassBCadActive before calling benchPassBCadRecordResult), never
// for production Sweep's own Pass B, so this can't affect or be confused
// with real Sweep data.
void benchPassBCadRecordResult(EnergyObservationResult result);
bool benchPassBCadLastResult(EnergyObservationResult &result);
