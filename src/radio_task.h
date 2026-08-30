#pragma once
// LoRaTrace RX — radio task (Core 1, highest priority).
//
// Owns the SX1262 exclusively and implements HOME_LISTEN from docs/DESIGN.md §5:
// continuous RX locked to the active profile's channel; on a valid packet,
// push a Detection into the queue and stay locked. Phase 4 adds the other
// half of §5's state machine text — "operator-selected... mutually
// exclusive" — as a live retune via radioRequestProfileSwitch(), below.
// DISCOVERY_SWEEP is implemented as the radio-owned bounded Probe path below.
// ENERGY_SWEEP remains Phase 9 and is deliberately absent here.
//
// The one hard rule (docs/DESIGN.md §2, CLAUDE.md): this task never touches SD
// or the display, and never blocks on another task. It reads the packet,
// re-arms RX, drops a ~36-byte struct in a queue, and goes back to
// listening. If the queue is full it *drops the detection and counts it*
// rather than waiting — a receiver that stalls to preserve a log entry is
// strictly worse than one that misses the entry.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "channel_plans.h"
#include "detection.h"
#include "energy_observation.h"
#include "node_identity.h"
#include "scan_observation.h"

// Starts the SX1262 on `channel`/`profile` and launches the task on Core 1.
// `queue` receives Detection structs, `scanQueue` receives fixed CAD
// observations, and `energyQueue` receives fixed energy-peak observations
// from Sweep; all three must outlive the task. `overrides`
// is the per-profile SD/web config main.cpp already loaded (config.h) —
// copied in and held for the task's lifetime so a later
// radioRequestProfileSwitch() resolves each profile's *current* override
// rather than always falling back to channel_plans.h's hardcoded table
// (the pre-2026-08-24 bug: switching away from a profile and back silently
// dropped its override — see docs/history/CHANGELOG.md).
// Returns false if the radio failed to initialise or the task couldn't be
// created — callers should treat that as fatal, since a wardriver with no
// receiver has nothing to do.
bool radioTaskStart(const ChannelParams &channel, MissionProfile profile,
                    const ProfileOverrides &overrides, QueueHandle_t queue,
                    QueueHandle_t scanQueue, QueueHandle_t energyQueue,
                    QueueHandle_t identityQueue);

void radioIdentityCaptureSetEnabled(bool enabled);
bool radioIdentityCaptureIsEnabled();
uint32_t radioIdentityDecodeCount();
uint32_t radioIdentityDropCount();

// Last RadioLib error code from begin()/startReceive() — including a live
// profile switch's own begin() call, so a failed switch is visible the same
// way a failed boot is.
int radioLastError();

// The mission profile HOME_LISTEN is actually locked to right now. Updated
// the instant a requested switch (below) actually lands on the radio, not
// the instant it's requested — same "small POD, no lock" caveat as
// radioActiveChannel() below.
MissionProfile radioActiveProfile();

// docs/DESIGN.md §5's operator-selected, mutually-exclusive profile switch:
// Meshtastic and MeshCore never listen at once. Queues a retune to
// `profile`'s channel table (channel_plans.h) and wakes the radio task to
// pick it up between packets — never blocks, so it's safe to call from
// ui_task's keyboard poll. A packet mid-flight on the old channel at the
// instant of the switch is lost; that's the accepted cost of "mutually
// exclusive," not a bug. Returns false if the radio task hasn't started yet.
bool radioRequestProfileSwitch(MissionProfile profile);

// Trace pause/standby (System menu, ui_task.cpp): puts the SX1262 into its
// warm sleep mode (radio.sleep(true) — retains config, no re-begin() needed
// to resume) instead of continuous RX, so no Detections are produced while
// paused. Same one-slot-mailbox, never-blocks contract as
// radioRequestProfileSwitch() above. GPS is deliberately untouched by this —
// see io_expander.h: GPS power shares the antenna-switch IO-expander line,
// so there's no independent GPS power to save here, and keeping it running
// means position is already fresh the instant Trace resumes. Returns false
// if the radio task hasn't started yet.
bool radioRequestTracePause(bool paused);

// Whether the radio is currently paused. Same small-POD, no-lock convention
// as radioActiveProfile() etc.
bool radioIsTracePaused();

// Starts a durable Probe sweep for the active profile. The radio task owns
// every retune/CAD operation; this call only writes a one-slot request and
// wakes that task. Calling it while a sweep is active requests cancellation.
bool radioRequestDiscoverySweep();
bool radioDiscoverySweepIsActive();
uint8_t radioDiscoveryCandidateIndex();
uint8_t radioDiscoveryCandidateCount();

// The most recent Probe's terminal state and its CAD result counts. These
// are a compact radio-owned summary for the on-device results page; detailed
// observations remain durable in probe.csv.
enum class DiscoverySweepState : uint8_t {
    IDLE,
    RUNNING,
    COMPLETE,
    CANCELLED,
    FAILED,
};
DiscoverySweepState radioDiscoverySweepState();
uint16_t radioDiscoveryCadFreeCount();
uint16_t radioDiscoveryCadDetectedCount();
// Bit N is set when candidate N produced CAD_DETECTED in the latest Probe.
// Plans are capped below sixteen entries, so this stays a small fixed value.
uint16_t radioDiscoveryCadDetectedMask();
uint16_t radioDiscoveryCadTimeoutCount();
uint16_t radioDiscoveryErrorCount();

// Starts a bounded Pass-A energy sweep (Phase 9, docs/DESIGN.md §5's
// `ENERGY_SWEEP`): the radio task retunes across every frequency bin
// (energy_plan.h), samples RSSI, and logs threshold-filtered peaks to a
// separate fixed queue/file (energy_observation.h) — never CAD, never a
// packet. Mutually exclusive with Probe: refuses while a Probe is active,
// and radioRequestDiscoverySweep() likewise refuses while a Sweep is
// active. Calling it while a sweep is active requests cancellation, same
// convention as radioRequestDiscoverySweep().
bool radioRequestEnergySweep();
bool radioEnergySweepIsActive();

// R key, distinct from S (operator request, 2026-08-29; moved off a Ctrl+S
// chord to its own dedicated key 2026-08-30 — see keyboard.h): starts/stops
// a chain of back-to-back Sweeps run one after another until cancelled,
// instead of one bounded pass — a "walk around and scan" field mode,
// distinct from the bounded single-shot check a plain tap already does.
// radioEnergySweepRepeatCount() is the lap counter (resets to 0 each time
// repeat mode starts), for the Sweep page's own repeat indicator.
bool radioRequestEnergySweepRepeat();
bool radioEnergySweepRepeatIsActive();
uint32_t radioEnergySweepRepeatCount();

uint16_t radioEnergyBinIndex();
uint16_t radioEnergyBinCount();
// Peaks logged during the most recent sweep (resets to 0 at the start of
// each run) — distinct from the cumulative radioEnergyObservationCount()
// below.
uint16_t radioEnergyPeakCount();

// True if `bin` was logged as a peak during the most recent sweep — a
// UI-facing occupancy sketch (e.g. tick marks along a frequency bar), not
// acquisition state. Always false for a bin index outside the current
// sweep's fixed 224-bit mask.
bool radioEnergyPeakBinSet(uint16_t bin);

// The strongest peak observed during the most recent sweep, for a single
// "most interesting thing found" UI callout. `valid` is false until at
// least one peak has been logged this sweep.
struct EnergyStrongestPeak {
    float freq_mhz = 0.0f;
    int16_t rssi_peak_dbm_x10 = 0;
    bool valid = false;
};
EnergyStrongestPeak radioEnergyStrongestPeak();

// Same terminal-state shape as DiscoverySweepState, kept as its own type:
// Sweep and Probe are different operations even though both resolve to
// one of these five outcomes.
enum class EnergySweepState : uint8_t {
    IDLE,
    RUNNING,
    COMPLETE,
    CANCELLED,
    FAILED,
};
EnergySweepState radioEnergySweepState();

uint32_t radioEnergyObservationCount();
uint32_t radioEnergyObservationDropCount();
uint32_t radioEnergySweepCount();
uint32_t radioEnergyCancelCount();
uint32_t radioEnergyFailureCount();
uint32_t radioEnergyRecoveryCount();
uint32_t radioEnergyLastAwayMs();

// Phase 9 Pass B (research/phase9-sweep-pass-b-design.md): CAD attempts run
// at the first PASS_B_MAX_PEAKS_PER_SWEEP Pass-A peaks this sweep, and how
// many of those attempts promoted a real packet to Detection
// (off_grid = true). Cumulative across sweeps, same convention as the
// other counters on this page.
uint32_t radioPassBAttemptCount();
uint32_t radioPassBDetectionCount();

// Bench-image-only (bench_fault.h's benchPassBCadTriggerAllowed()): runs
// one Pass B CAD attempt on demand at a fixed test frequency, for the
// false-positive-vs-SF bench matrix (research/phase9-sweep-pass-b-design.md).
// comboIndex selects a row of PASS_B_SF_BW_CANDIDATES (pass_b_plan.h).
// Refuses (returns false) outside the bench build, with a bad index, or
// while any radio-owning action (Watch pause aside) is already active.
bool radioRequestBenchPassBCadTrigger(uint8_t comboIndex);
bool radioBenchPassBCadIsActive();

// --- Diagnostics -------------------------------------------------------
// Exposed because Phase 2's exit criterion is "no dropped packets
// attributable to SD latency" — that claim is only checkable if drops are
// counted rather than silently absorbed.
uint32_t radioPacketCount();     // successfully decoded
uint32_t radioCrcErrorCount();   // received but failed CRC
uint32_t radioQueueDropCount();  // decoded but the queue was full
uint32_t radioBusMissCount();    // couldn't get the SPI bus in time
uint32_t radioScanObservationDropCount();
uint32_t radioScanObservationCount();
uint32_t radioDiscoverySweepCount();
uint32_t radioDiscoveryCancelCount();
uint32_t radioDiscoveryTimeoutCount();
uint32_t radioDiscoveryFailureCount();
uint32_t radioDiscoveryRecoveryCount();
uint32_t radioDiscoveryLastAwayMs();

// The channel table HOME_LISTEN is locked to right now (post SD-config-
// override at boot; post radioRequestProfileSwitch() if one has landed
// since — that's still the only hot-reload path, config.txt itself is still
// boot-time-only, see config.h). A read-only copy, safe to call from any
// task: wifi_task's settings page uses this to show the current values, not
// just the last thing config.txt said, since a bad/missing SD card at boot
// or a runtime profile switch both mean those can differ.
ChannelParams radioActiveChannel();

// The per-profile SD/web overrides loaded at boot (config.h), exactly as
// radioTaskStart() received them — this task's copy is the one
// radioRequestProfileSwitch() actually resolves against, so wifi_task's
// settings page reads from here rather than re-parsing config.txt itself
// (which would risk disagreeing with what the radio is actually doing).
// Same small-POD, no-lock convention as radioActiveChannel() above. Does
// NOT reflect a save made through writeProfileConfigToSD() until the next
// boot — same "not live" boundary as the channel override itself.
ProfileOverrides radioActiveOverrides();
