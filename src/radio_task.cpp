#include "radio_task.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board_pins.h"
#include "bench_fault.h"
#include "cell_plan.h"
#include "discovery_plan.h"
#include "energy_plan.h"
#include "memory_stats.h"
#include "meshcore_identity.h"
#include "meshtastic_identity.h"
#include "pass_b_plan.h"
#include "spi_bus.h"

namespace {

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, sharedSpi());

TaskHandle_t radioTaskHandle = nullptr;
QueueHandle_t detectionQueue = nullptr;
QueueHandle_t scanObservationQueue = nullptr;
QueueHandle_t identityQueue = nullptr;
ChannelParams activeChannel;
MissionProfile activeProfile = MissionProfile::MESHTASTIC;
// Per-profile SD/web overrides, copied in once at radioTaskStart() and
// read-only after that — every radioRequestProfileSwitch() resolves
// against this same copy, which is what keeps a switch from reverting to
// the hardcoded default (channel_plans.h).
ProfileOverrides activeOverrides;

// One-slot mailbox for radioRequestProfileSwitch(): xQueueOverwrite always
// leaves only the most recent request in place, so a caller firing this
// twice in a row (a bouncy key) can't queue up a switch-then-switch-back.
struct PendingSwitch {
    MissionProfile profile;
    ChannelParams channel;
};
QueueHandle_t profileSwitchQueue = nullptr;

// Same one-slot-mailbox pattern, for Trace pause/standby
// (radioRequestTracePause() below). tracePaused mirrors what the radio
// task's own loop has actually done (radio.sleep()/startReceive()), not
// just what was requested — same convention activeProfile follows.
QueueHandle_t pauseQueue = nullptr;
volatile bool tracePaused = false;

// Probe uses a one-slot start mailbox plus a cancellation flag. The scan
// itself runs only on this task, so the flag needs no lock and cancellation
// cannot race a second radio owner.
QueueHandle_t discoveryQueue = nullptr;
volatile bool discoveryActive = false;
volatile bool discoveryCancelRequested = false;
volatile uint8_t discoveryCandidateIndex = 0;
volatile uint8_t discoveryCandidateCount = 0;
volatile DiscoverySweepState discoveryState = DiscoverySweepState::IDLE;
volatile uint16_t discoveryCadFreeCount = 0;
volatile uint16_t discoveryCadDetectedCount = 0;
volatile uint16_t discoveryCadDetectedMask = 0;
volatile uint16_t discoveryCadTimeoutCount = 0;
volatile uint16_t discoveryErrorCount = 0;

// Sweep (ENERGY_SWEEP, Phase 9) mirrors Probe's one-slot-mailbox-plus-
// cancellation-flag shape exactly — see the discovery* block above. Mutual
// exclusion with Probe is enforced in radioRequestEnergySweep()/
// radioRequestDiscoverySweep(), not here.
QueueHandle_t energySweepQueue = nullptr;
QueueHandle_t energyObservationQueue = nullptr;
volatile bool energyActive = false;
volatile bool energyCancelRequested = false;
// Repeat-Sweep mode (R key, operator request 2026-08-29; moved off a
// Ctrl+S chord to its own dedicated key 2026-08-30 — see keyboard.h):
// re-runs
// performEnergySweep() back-to-back after each COMPLETE instead of
// stopping after one bounded pass — "walk around and scan" field use,
// distinct from the bounded single-shot war-drive check a tap already
// does. energyRepeatCount is the lap counter the Sweep page displays;
// reset to 0 each time repeat mode starts (radioRequestEnergySweepRepeat()).
volatile bool energyRepeatActive = false;
volatile uint32_t energyRepeatCount = 0;
// Region constrains Sweep's own band (energySweepBandForRegion(),
// energy_plan.h) — set once at boot from SD (region_settings.h) and again
// whenever the operator cycles System > Region, read once at the start of
// performEnergySweep(), never mid-sweep.
volatile Region activeEnergySweepRegion = Region::US;
volatile uint16_t energyBinIndex = 0;
volatile uint16_t energyTotalBins = 0;
volatile EnergySweepState energyState = EnergySweepState::IDLE;
volatile uint16_t energyPeakCount = 0;
// Bit N set if bin N was a peak in the most recent sweep — a UI-facing
// occupancy sketch, not acquisition state (Pass A never reads this back).
// 28 bytes covers ENERGY_BIN_RESERVED_COUNT's full 224 bits. Live: reset at
// the top of performEnergySweep() and filled in bin-by-bin as the scan
// progresses, so drawSweepOccupancy() (ui_pages.cpp) can render ticks
// appearing progressively during an active sweep.
uint8_t energyPeakBinMask[28] = {};
// A stable copy of the mask above, taken once a sweep finishes (right where
// energySweepCount++ fires) — analyzer_state.cpp's analyzerNoteSweepComplete()
// reads this one instead, not the live mask. Necessary because in repeat
// mode radio_task's own do-while loops straight back into
// performEnergySweep() for the next lap, whose first line resets the live
// mask -- with no delay before that reset, logger_task's ~100ms cross-core
// poll of radioEnergySweepCount() (Core 0) almost always lost that race
// against Core 1, so every Waterfall row in repeat mode read an
// already-cleared mask and reported quiet regardless of what Pass A
// actually found (found live on hardware 2026-09-03: PBA=50 cumulative
// Pass-B triggers over a repeat run, Waterfall showing nothing the whole
// time). energy.csv itself was never affected -- enqueueEnergyObservation()
// pushes each peak to a queue the instant Pass A finds it, mid-sweep,
// independent of this mask entirely.
uint8_t energyPeakBinMaskAtComplete[28] = {};
// The strongest peak observed this sweep, for the UI's single "most
// interesting thing found" callout — cheap to keep (one float + one
// int16) precisely because it's a running max, not a per-bin history.
volatile float energyStrongestFreqMhz = 0.0f;
volatile int16_t energyStrongestRssiDbmX10 = 0;
volatile bool energyStrongestValid = false;

int lastError = RADIOLIB_ERR_NONE;

volatile uint32_t packetCount = 0;
volatile uint32_t crcErrorCount = 0;
volatile uint32_t queueDropCount = 0;
volatile bool identityCaptureEnabled = true;
volatile uint32_t identityDecodeCount = 0;
volatile uint32_t identityDropCount = 0;
volatile uint32_t busMissCount = 0;
volatile uint32_t scanObservationCount = 0;
volatile uint32_t scanObservationDropCount = 0;
volatile uint32_t discoverySweepCount = 0;
volatile uint32_t discoveryCancelCount = 0;
volatile uint32_t discoveryTimeoutCount = 0;
volatile uint32_t discoveryFailureCount = 0;
volatile uint32_t discoveryRecoveryCount = 0;
volatile uint32_t discoveryLastAwayMs = 0;

volatile uint32_t energyObservationCount = 0;
volatile uint32_t energyObservationDropCount = 0;
volatile uint32_t energySweepCount = 0;
volatile uint32_t energyCancelCount = 0;
volatile uint32_t energyFailureCount = 0;
volatile uint32_t energyRecoveryCount = 0;
volatile uint32_t energyLastAwayMs = 0;

// Phase 9 Pass B (research/phase9-sweep-pass-b-design.md). Cumulative
// across sweeps, same convention as the counters above.
volatile uint32_t passBAttemptCount = 0;
volatile uint32_t passBDetectionCount = 0;

// Cell mirrors Probe/Sweep's one-slot-mailbox-plus-cancellation-flag
// shape exactly (see the discovery*/energy* blocks above). Mutual exclusion
// with Probe and Sweep is enforced in radioRequestCellSweep()/
// radioRequestDiscoverySweep()/radioRequestEnergySweep(), not here.
QueueHandle_t cellSweepQueue = nullptr;
QueueHandle_t cellObservationQueue = nullptr;
volatile bool cellActive = false;
volatile bool cellCancelRequested = false;
// Repeat-Cell mode (R key, page-gated to the Cell card by ui_task.cpp):
// re-runs performCellSweep() back-to-back after each COMPLETE, same "walk
// around and scan" shape as energyRepeatActive/energyRepeatCount above,
// mirrored exactly. Probe has no equivalent (operator decision: "Repeat
// only on the Sweeps").
volatile bool cellRepeatActive = false;
volatile uint32_t cellRepeatCount = 0;
volatile uint16_t cellBinIndexState = 0;
volatile uint16_t cellTotalBins = 0;
volatile CellSweepState cellState = CellSweepState::IDLE;
volatile float cellStrongestFreqMhz = 0.0f;
volatile int16_t cellStrongestRssiDbmX10 = 0;
volatile bool cellStrongestValid = false;

volatile uint32_t cellObservationCount = 0;
volatile uint32_t cellObservationDropCount = 0;
volatile uint32_t cellSweepCount = 0;
volatile uint32_t cellCancelCount = 0;
volatile uint32_t cellFailureCount = 0;
volatile uint32_t cellRecoveryCount = 0;
volatile uint32_t cellLastAwayMs = 0;

// Field Analyzer Scope (Phase 10, docs/research/LoRaTrace-Phases-7-10-
// Design.md §8.2/§8.4): mirrors Cell/Sweep/Probe's one-slot-mailbox-plus-
// cancellation-flag shape (see the discovery*/energy*/cell* blocks above).
// Mutual exclusion with Probe/Sweep/Cell is enforced in
// radioRequestScopeAcquire(), not here. Unlike those three, its result
// isn't a small POD read after the fact — it's a live-growing ScopeTrace
// another task reads mid-acquisition (§8.4's "UI copies a snapshot"), so it
// needs its own mutex — gps_task.cpp's fixMutex/sharedFix shape exactly.
QueueHandle_t scopeAcquireQueue = nullptr;
volatile bool scopeActive = false;
volatile bool scopeCancelRequested = false;
volatile ScopeAcquireState scopeState = ScopeAcquireState::IDLE;
SemaphoreHandle_t scopeTraceMutex = nullptr;
ScopeTrace sharedScopeTrace; // guarded by scopeTraceMutex

volatile uint32_t scopeAcquireCount = 0;
volatile uint32_t scopeCancelCount = 0;
volatile uint32_t scopeFailureCount = 0;
volatile uint32_t scopeRecoveryCount = 0;
volatile uint32_t scopeLastAwayMs = 0;

// Bench-only on-demand single-combo CAD trigger (research/
// phase9-sweep-pass-b-design.md's false-positive-vs-SF bench matrix). One-
// slot mailbox holding a PASS_B_SF_BW_CANDIDATES index, same shape as
// discoveryQueue/energySweepQueue above.
QueueHandle_t benchPassBCadQueue = nullptr;
volatile bool benchPassBCadActive = false;
// MESH_OREGON's frequency -- the same fixed target
// scripts/phase8_cad_rate_bench.py already uses, so this bench reuses an
// established, already-characterized test point rather than an arbitrary
// new frequency.
constexpr float BENCH_PASS_B_CAD_TEST_FREQ_MHZ = 918.5f;

// Bench-only on-demand park-and-sample RSSI window (docs/STATUS.md's
// 923MHz-edge injected-carrier characterization). One-slot mailbox holding
// a target frequency in kHz, same shape as benchPassBCadQueue above.
QueueHandle_t benchRssiWindowQueue = nullptr;
volatile bool benchRssiWindowActive = false;
volatile bool benchRssiWindowHaveResult = false;
volatile int16_t benchRssiWindowMaxDbmX10 = 0;
volatile int16_t benchRssiWindowAvgDbmX10 = 0;
volatile uint16_t benchRssiWindowSampleCount = 0;
// ~2s window (400 samples * 5ms) -- long enough that a transmitter firing
// every few hundred ms during it is very likely to have at least one burst
// land inside, unlike a Sweep bin's own tens-of-ms dwell.
constexpr uint16_t BENCH_RSSI_WINDOW_SAMPLE_COUNT = 400;
constexpr uint32_t BENCH_RSSI_WINDOW_SAMPLE_INTERVAL_MS = 5;

// How long the radio task waits for the shared SPI bus. Generous enough to
// ride out a normal SD flush, short enough that a wedged logger can't take
// the receiver down with it. On timeout the packet is dropped and RX keeps
// listening.
constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(250);
constexpr uint32_t DISCOVERY_CAD_TIMEOUT_MS = 300;
constexpr uint32_t DISCOVERY_RX_WINDOW_MS = 2500;
// Placeholders pending real calibration (energy_observation.h carries the
// same caveat for the noise-floor margin/divisor): 4 samples spaced 1ms
// apart per bin keeps a full 221-bin sweep in the few-second range Probe's
// own CAD sweep already established as a reasonable bounded-scan duration.
constexpr uint8_t ENERGY_SAMPLES_PER_BIN = 4;
constexpr uint32_t ENERGY_SAMPLE_INTERVAL_MS = 1;
// One full trace per bounded Scope acquisition: SCOPE_MAX_SAMPLES samples
// at this spacing is ~4.8s — in the same few-second bounded-scan range
// Cell/Sweep/Probe already established as reasonable for a field
// instrument.
constexpr uint32_t SCOPE_SAMPLE_INTERVAL_MS = 20;

uint8_t discoveryCadSymbolConfig() {
    switch (benchCadSymbols()) {
        case 1: return RADIOLIB_SX126X_CAD_ON_1_SYMB;
        case 4: return RADIOLIB_SX126X_CAD_ON_4_SYMB;
        case 8: return RADIOLIB_SX126X_CAD_ON_8_SYMB;
        case 16: return RADIOLIB_SX126X_CAD_ON_16_SYMB;
        case 2:
        default: return RADIOLIB_SX126X_CAD_ON_2_SYMB;
    }
}

// DIO1 fires on RX-done. Keep this to a notification and nothing else: no
// SPI, no Serial, no allocation. RadioLib requires the ISR be IRAM-safe.
void IRAM_ATTR onDio1Action() {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (radioTaskHandle != nullptr) {
        vTaskNotifyGiveFromISR(radioTaskHandle, &higherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

// Reads one completed packet while the caller holds the SPI bus. Reusing
// this path for Watch and Probe keeps packet-bearing CAD hits on the same
// Detection pipeline and preserves the same read-before-rearm ordering.
bool readDetectionLocked(const ChannelParams &channel, MissionProfile profile,
                         uint8_t *buf, size_t bufSize, Detection &det,
                         bool &readFailed) {
    readFailed = false;
    det = {};

    const size_t len = radio.getPacketLength();
    int state = RADIOLIB_ERR_NONE;
    if (len > 0 && len <= bufSize) {
        state = radio.readData(buf, len);
    } else {
        state = RADIOLIB_ERR_UNKNOWN;
    }

    float rssi = 0.0f, snr = 0.0f;
    if (state == RADIOLIB_ERR_NONE) {
        // GetPacketStatus reports the last packet, so read it before RX is
        // re-armed and another arrival can overwrite it.
        rssi = radio.getRSSI();
        snr = radio.getSNR();
    }
    radio.startReceive();

    if (state != RADIOLIB_ERR_NONE) {
        readFailed = true;
        return false;
    }

    det.rx_millis = millis();
    det.freq_mhz = channel.freq_mhz;
    det.rssi_dbm = rssi;
    det.snr_db = snr;
    if (!detectionSetRawPacket(det, buf, len)) {
        readFailed = true;
        return false;
    }
    det.bw_khz_x10 = (uint16_t)(channel.bw_khz * 10.0f + 0.5f);
    det.sf = channel.sf;
    det.cr_denom = channel.cr_denom;
    det.sync_word = channel.sync_word;
    det.profile = (uint8_t)profile;

    if (profile == MissionProfile::MESHTASTIC) {
        detectionApplyMeshtasticHeader(det, buf, len);
    }

    packetCount++;
    return true;
}

void enqueueDetection(const Detection &det) {
    if (detectionQueue == nullptr) return;
    if (xQueueSend(detectionQueue, &det, 0) != pdTRUE) queueDropCount++;
}

void enqueueNodeIdentity(const Detection &det) {
    if (!identityCaptureEnabled || identityQueue == nullptr) return;
    NodeIdentity identity;
    const bool decoded = det.profile == (uint8_t)MissionProfile::MESHTASTIC
        ? meshtasticDecodeDefaultNodeIdentity(det, identity)
        : det.profile == (uint8_t)MissionProfile::MESHCORE
            ? meshcoreDecodeAdvertIdentity(det, identity)
            : false;
    if (!decoded) return;
    identityDecodeCount++;
    if (xQueueSend(identityQueue, &identity, 0) != pdTRUE) identityDropCount++;
}

void enqueueScanObservation(const DiscoveryCandidate &candidate, uint8_t candidateIndex,
                            ScanObservationResult result, int16_t radioStatus) {
    ScanObservation observation;
    observation.rx_millis = millis();
    observation.freq_mhz = candidate.channel.freq_mhz;
    observation.radio_status = radioStatus;
    observation.bw_khz_x10 = (uint16_t)(candidate.channel.bw_khz * 10.0f + 0.5f);
    observation.sf = candidate.channel.sf;
    observation.cr_denom = candidate.channel.cr_denom;
    observation.sync_word = candidate.channel.sync_word;
    observation.profile = (uint8_t)activeProfile;
    observation.candidate_index = candidateIndex;
    observation.result = result;

    switch (result) {
        case ScanObservationResult::CAD_FREE: discoveryCadFreeCount++; break;
        case ScanObservationResult::CAD_DETECTED:
            discoveryCadDetectedCount++;
            if (candidateIndex < 16) discoveryCadDetectedMask |= (uint16_t)(1U << candidateIndex);
            break;
        case ScanObservationResult::CAD_TIMEOUT: discoveryCadTimeoutCount++; break;
        case ScanObservationResult::RADIO_ERROR: discoveryErrorCount++; break;
        default: break;
    }

    scanObservationCount++;
    if (scanObservationQueue == nullptr ||
        xQueueSend(scanObservationQueue, &observation, 0) != pdTRUE) {
        scanObservationDropCount++;
    }
}

bool discoveryAbortPending() {
    if (discoveryCancelRequested) return true;
    if (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) return true;
    if (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0) return true;
    return false;
}

// A threshold-filtered peak only — Pass A never enqueues a non-peak bin
// (docs/DESIGN.md §8.1: "don't dump every sweep point, only peaks"). `wifi_on`
// is hardcoded false: a real WiFi-state getter is a later slice's concern
// (this task has no WiFi dependency today and shouldn't grow one just to
// answer this field).
void enqueueEnergyObservation(uint16_t binIndex, const ChannelParams &channel,
                              const EnergyBinStats &stats) {
    EnergyObservation observation;
    observation.rx_millis = millis();
    observation.freq_mhz = energyBinFrequencyMhz(
        binIndex, energySweepBandForRegion(activeEnergySweepRegion), ENERGY_SWEEP_DEFAULT_STEP);
    observation.bw_khz_x10 = (uint16_t)(channel.bw_khz * 10.0f + 0.5f);
    observation.bin_step_khz = energyBinStepKhz(ENERGY_SWEEP_DEFAULT_STEP);
    observation.rssi_avg_dbm_x10 = stats.rssi_avg_dbm_x10;
    observation.rssi_peak_dbm_x10 = stats.rssi_peak_dbm_x10;
    observation.radio_status = 0;
    observation.profile = (uint8_t)activeProfile;
    observation.bin_index = (uint8_t)binIndex;
    observation.sf = channel.sf;
    observation.cr_denom = channel.cr_denom;
    observation.sync_word = channel.sync_word;
    observation.sample_count = stats.sample_count;
    observation.result = EnergyObservationResult::ENERGY_PEAK;
    observation.packet_metadata_present = false;
    observation.wifi_on = false;

    energyPeakCount++;
    energyObservationCount++;
    if (energyObservationQueue == nullptr ||
        xQueueSend(energyObservationQueue, &observation, 0) != pdTRUE) {
        energyObservationDropCount++;
    }
}

// One Pass B CAD attempt at a Pass-A peak bin (research/
// phase9-sweep-pass-b-design.md). Unlike enqueueEnergyObservation() this
// logs every attempt, not peaks only -- a bounded, sparse-by-construction
// set (PASS_B_MAX_PEAKS_PER_SWEEP x PASS_B_SF_BW_CANDIDATE_COUNT per
// sweep), so the "log peaks only" storage argument (docs/DESIGN.md §8.1) that
// motivates Pass A's own filtering doesn't apply here. rssi fields stay
// 0: Pass B doesn't re-measure RSSI, only CAD/receive outcomes at a bin
// Pass A already measured.
void enqueuePassBObservation(uint16_t binIndex, const PassBModemParams &combo,
                              EnergyObservationResult result, int16_t radioStatus,
                              bool packetMetadataPresent) {
    EnergyObservation observation;
    observation.rx_millis = millis();
    observation.freq_mhz = energyBinFrequencyMhz(
        binIndex, energySweepBandForRegion(activeEnergySweepRegion), ENERGY_SWEEP_DEFAULT_STEP);
    observation.bw_khz_x10 = (uint16_t)(combo.bw_khz * 10.0f + 0.5f);
    observation.bin_step_khz = energyBinStepKhz(ENERGY_SWEEP_DEFAULT_STEP);
    observation.rssi_avg_dbm_x10 = 0;
    observation.rssi_peak_dbm_x10 = 0;
    observation.radio_status = radioStatus;
    observation.profile = (uint8_t)activeProfile;
    observation.bin_index = (uint8_t)binIndex;
    observation.sf = combo.sf;
    observation.cr_denom = combo.cr_denom;
    observation.sync_word = combo.sync_word;
    observation.sample_count = 0;
    observation.result = result;
    observation.packet_metadata_present = packetMetadataPresent;
    observation.wifi_on = false;

    passBAttemptCount++;
    energyObservationCount++;
    if (energyObservationQueue == nullptr ||
        xQueueSend(energyObservationQueue, &observation, 0) != pdTRUE) {
        energyObservationDropCount++;
    }

    // Bench-only (bench_fault.h): only for a BENCH_PASS_B_CAD-triggered
    // attempt, never production Sweep's own Pass B -- benchPassBCadActive
    // is only ever true inside performBenchPassBCadTrigger()'s call.
    if (benchPassBCadActive) {
        benchPassBCadRecordResult(result);
    }
}

bool energyAbortPending() {
    if (energyCancelRequested) return true;
    if (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) return true;
    if (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0) return true;
    return false;
}

bool cellAbortPending() {
    if (cellCancelRequested) return true;
    if (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) return true;
    if (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0) return true;
    return false;
}

bool scopeAbortPending() {
    if (scopeCancelRequested) return true;
    if (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) return true;
    if (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0) return true;
    return false;
}

// Every bin, not peak-filtered — see cell_observation.h's file header for
// why Cell doesn't borrow ENERGY_SWEEP's calibrated threshold. 101
// bins/sweep keeps this an honest, modest CSV rather than a RAM concern.
void enqueueCellObservation(uint16_t binIndex, const ChannelParams &channel,
                            const EnergyBinStats &stats, CellObservationResult result,
                            int16_t radioStatus) {
    CellObservation observation;
    observation.rx_millis = millis();
    observation.freq_mhz = cellBinFrequencyMhz(binIndex);
    observation.rx_bw_khz_x10 = (uint16_t)(channel.bw_khz * 10.0f + 0.5f);
    observation.rssi_avg_dbm_x10 = stats.rssi_avg_dbm_x10;
    observation.rssi_peak_dbm_x10 = stats.rssi_peak_dbm_x10;
    observation.radio_status = radioStatus;
    observation.profile = (uint8_t)activeProfile;
    observation.bin_index = (uint8_t)binIndex;
    observation.sample_count = stats.sample_count;
    observation.result = result;

    if (result == CellObservationResult::MEASURED && stats.sample_count > 0 &&
        (!cellStrongestValid || stats.rssi_peak_dbm_x10 > cellStrongestRssiDbmX10)) {
        cellStrongestFreqMhz = observation.freq_mhz;
        cellStrongestRssiDbmX10 = stats.rssi_peak_dbm_x10;
        cellStrongestValid = true;
    }

    cellObservationCount++;
    if (cellObservationQueue == nullptr ||
        xQueueSend(cellObservationQueue, &observation, 0) != pdTRUE) {
        cellObservationDropCount++;
    }
}

// abortPending defaults to discoveryAbortPending so Probe's two existing
// call sites are untouched; Pass B (below) passes energyAbortPending
// explicitly, since a Sweep-cancel request during Pass B's own CAD/RX
// waits must be checked against Sweep's cancel flag, not Probe's.
bool waitForDioUntil(uint32_t timeoutMs, bool (*abortPending)() = discoveryAbortPending) {
    const uint32_t started = millis();
    for (;;) {
        if (digitalRead(PIN_LORA_IRQ)) return true;
        if (abortPending()) return false;
        if (millis() - started >= timeoutMs) return false;
        vTaskDelay(1);
    }
}

bool applyBenchFault(BenchFaultPoint point, const DiscoveryCandidate *candidate,
                     uint8_t candidateIndex, bool &aborted, bool &failed) {
    BenchFaultAction action;
    if (!benchFaultTake(point, action)) return false;
    if (action == BenchFaultAction::CANCEL) {
        discoveryCancelRequested = true;
        aborted = true;
    } else {
        lastError = RADIOLIB_ERR_UNKNOWN;
        if (candidate != nullptr) {
            enqueueScanObservation(*candidate, candidateIndex, ScanObservationResult::RADIO_ERROR,
                                   RADIOLIB_ERR_UNKNOWN);
        }
        failed = true;
    }
    return true;
}

bool restoreHomeListen(const ChannelParams &homeChannel, MissionProfile homeProfile) {
    SpiBusLock lock(BUS_WAIT);
    if (!lock.held()) {
        busMissCount++;
        return false;
    }

    const int beginState = radio.begin(homeChannel.freq_mhz, homeChannel.bw_khz,
                                       homeChannel.sf, homeChannel.cr_denom,
                                       homeChannel.sync_word);
    if (beginState != RADIOLIB_ERR_NONE) {
        lastError = beginState;
        return false;
    }

    lastError = radio.startReceive();
    if (lastError != RADIOLIB_ERR_NONE) return false;
    activeChannel = homeChannel;
    activeProfile = homeProfile;
    return true;
}

void performDiscoverySweep() {
    if (discoveryActive || cellActive || scopeActive) return;

    discoveryActive = true;
    discoveryState = DiscoverySweepState::RUNNING;
    discoveryCancelRequested = false;
    const uint32_t awayStarted = millis();
    const ChannelParams homeChannel = activeChannel;
    const MissionProfile homeProfile = activeProfile;
    const DiscoveryPlan plan = discoveryPlanForProfile(homeProfile);
    discoveryCandidateIndex = 0;
    discoveryCandidateCount = 0;
    discoveryCadFreeCount = 0;
    discoveryCadDetectedCount = 0;
    discoveryCadDetectedMask = 0;
    discoveryCadTimeoutCount = 0;
    discoveryErrorCount = 0;
    for (uint8_t i = 0; i < plan.count; i++) {
        if (!discoveryChannelEquals(plan.candidates[i].channel, homeChannel)) {
            discoveryCandidateCount++;
        }
    }
    bool aborted = false;
    bool failed = false;

    // A notification may be left by the home RX IRQ that woke the task to
    // service the Probe request. CAD is polled by the task below, so discard
    // only that stale wakeup before starting the first candidate.
    ulTaskNotifyTake(pdTRUE, 0);

    for (uint8_t i = 0; i < plan.count; i++) {
        if (discoveryAbortPending()) {
            aborted = true;
            break;
        }

        const DiscoveryCandidate &candidate = plan.candidates[i];
        if (discoveryChannelEquals(candidate.channel, homeChannel)) continue;
        discoveryCandidateIndex++;

        if (applyBenchFault(BenchFaultPoint::BEFORE_RETUNE, &candidate, i, aborted, failed)) {
            break;
        }

        int beginState;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                enqueueScanObservation(candidate, i, ScanObservationResult::RADIO_ERROR,
                                       RADIOLIB_ERR_SPI_CMD_TIMEOUT);
                failed = true;
                break;
            }
            beginState = radio.begin(candidate.channel.freq_mhz, candidate.channel.bw_khz,
                                     candidate.channel.sf, candidate.channel.cr_denom,
                                     candidate.channel.sync_word);
            if (beginState == RADIOLIB_ERR_NONE) {
                ChannelScanConfig_t config = {
                    .cad = {
                        .symNum = discoveryCadSymbolConfig(),
                        .detPeak = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                        .detMin = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                        .exitMode = RADIOLIB_SX126X_CAD_GOTO_STDBY,
                        .timeout = DISCOVERY_CAD_TIMEOUT_MS * 1000UL,
                        .irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS,
                        .irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK,
                    },
                };
                beginState = radio.startChannelScan(config);
            }
        }

        if (beginState != RADIOLIB_ERR_NONE) {
            lastError = beginState;
            enqueueScanObservation(candidate, i, ScanObservationResult::RADIO_ERROR,
                                   (int16_t)beginState);
            failed = true;
            break;
        }

        if (applyBenchFault(BenchFaultPoint::AFTER_RETUNE, &candidate, i, aborted, failed)) {
            break;
        }
        if (applyBenchFault(BenchFaultPoint::CAD_WAIT, &candidate, i, aborted, failed)) {
            break;
        }

        if (!waitForDioUntil(DISCOVERY_CAD_TIMEOUT_MS)) {
            if (discoveryAbortPending()) {
                aborted = true;
                break;
            }
            discoveryTimeoutCount++;
            enqueueScanObservation(candidate, i, ScanObservationResult::CAD_TIMEOUT,
                                   RADIOLIB_ERR_RX_TIMEOUT);
            continue;
        }

        // The CAD IRQ is consumed by the polling path rather than left to be
        // mistaken for a Watch packet after home configuration is restored.
        ulTaskNotifyTake(pdTRUE, 0);
        int16_t scanState;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                enqueueScanObservation(candidate, i, ScanObservationResult::RADIO_ERROR,
                                       RADIOLIB_ERR_SPI_CMD_TIMEOUT);
                failed = true;
                break;
            }
            scanState = (int16_t)radio.getChannelScanResult();
        }

        if (scanState != RADIOLIB_LORA_DETECTED && scanState != RADIOLIB_CHANNEL_FREE) {
            lastError = scanState;
            enqueueScanObservation(candidate, i, ScanObservationResult::RADIO_ERROR,
                                   scanState);
            failed = true;
            break;
        }

        enqueueScanObservation(candidate, i,
                               scanState == RADIOLIB_LORA_DETECTED
                                   ? ScanObservationResult::CAD_DETECTED
                                   : ScanObservationResult::CAD_FREE,
                               scanState);

        if (scanState != RADIOLIB_LORA_DETECTED) continue;

        // CAD is not a packet. Give a detected preamble a bounded receive
        // window so a valid packet still enters Detection rather than being
        // silently reduced to a scan-only hit.
        int receiveState;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                failed = true;
                break;
            }
            receiveState = radio.startReceive();
        }
        if (receiveState != RADIOLIB_ERR_NONE) {
            lastError = receiveState;
            enqueueScanObservation(candidate, i, ScanObservationResult::RADIO_ERROR,
                                   (int16_t)receiveState);
            failed = true;
            break;
        }

        if (applyBenchFault(BenchFaultPoint::RX_WAIT, &candidate, i, aborted, failed)) {
            break;
        }
        if (!waitForDioUntil(DISCOVERY_RX_WINDOW_MS)) {
            if (discoveryAbortPending()) {
                aborted = true;
                break;
            }
            continue;
        }

        ulTaskNotifyTake(pdTRUE, 0);
        uint8_t buf[DETECTION_RAW_MAX_LEN];
        Detection detection;
        bool readFailed;
        bool haveDetection = false;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                failed = true;
            } else {
                haveDetection = readDetectionLocked(candidate.channel, homeProfile, buf,
                                                    sizeof(buf), detection, readFailed);
                if (readFailed) crcErrorCount++;
            }
        }
        if (failed) break;
        if (haveDetection) {
            enqueueDetection(detection);
            enqueueNodeIdentity(detection);
        }
    }

    // CAD and RX IRQs share the task notification with profile/pause requests.
    // Drain only while still away from home, then re-wake the task if a
    // request mailbox is pending so a scan cannot consume its wakeup.
    const bool requestPending =
        (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) ||
        (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0);
    ulTaskNotifyTake(pdTRUE, 0);
    applyBenchFault(BenchFaultPoint::HOME_RESTORE_BEFORE, nullptr, 0, aborted, failed);
    const bool restored = restoreHomeListen(homeChannel, homeProfile);
    applyBenchFault(BenchFaultPoint::HOME_RESTORE_AFTER, nullptr, 0, aborted, failed);
    if (requestPending && radioTaskHandle != nullptr) xTaskNotifyGive(radioTaskHandle);
    if (!restored) failed = true;

    discoverySweepCount++;
    if (aborted || discoveryCancelRequested) discoveryCancelCount++;
    if (failed) discoveryFailureCount++;
    if (restored) discoveryRecoveryCount++;
    discoveryLastAwayMs = millis() - awayStarted;
    discoveryState = failed ? DiscoverySweepState::FAILED
                             : (aborted || discoveryCancelRequested
                                    ? DiscoverySweepState::CANCELLED
                                    : DiscoverySweepState::COMPLETE);
    discoveryCancelRequested = false;
    discoveryActive = false;
}

// Pass B (research/phase9-sweep-pass-b-design.md): runs CAD across the
// small sourced PASS_B_SF_BW_CANDIDATES table at one Pass-A peak bin.
// Called immediately when performEnergySweep()'s own loop finds a peak,
// not deferred to after the full sweep -- a 2026-08-28 revision after an
// operator observation that a deferred second pass can arrive after a
// brief transmitter has already gone quiet again. Reuses Probe's own CAD +
// bounded-receive-on-hit sequence (performDiscoverySweep() above) almost
// verbatim, at this bin's frequency instead of a curated candidate.
// One CAD attempt at one combo. Extracted from passBCadAtBin() (below) so
// the bench-only single-combo trigger (performBenchPassBCadTrigger(),
// research/phase9-sweep-pass-b-design.md's false-positive-vs-SF bench
// matrix) can run exactly this same sequence for one operator-chosen combo,
// without also running the other nine.
void passBCadOneCombo(uint16_t bin, float freq, const PassBModemParams &combo,
                      bool &aborted, bool &failed) {
    if (applyBenchFault(BenchFaultPoint::BEFORE_RETUNE, nullptr, 0, aborted, failed)) {
        return;
    }

    int beginState;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            busMissCount++;
            failed = true;
            return;
        }
        beginState = radio.begin(freq, combo.bw_khz, combo.sf, combo.cr_denom,
                                 combo.sync_word);
        if (beginState == RADIOLIB_ERR_NONE) {
            ChannelScanConfig_t config = {
                .cad = {
                    .symNum = discoveryCadSymbolConfig(),
                    .detPeak = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                    .detMin = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                    .exitMode = RADIOLIB_SX126X_CAD_GOTO_STDBY,
                    .timeout = DISCOVERY_CAD_TIMEOUT_MS * 1000UL,
                    .irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS,
                    .irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK,
                },
            };
            beginState = radio.startChannelScan(config);
        }
    }
    if (beginState != RADIOLIB_ERR_NONE) {
        lastError = beginState;
        enqueuePassBObservation(bin, combo, EnergyObservationResult::RADIO_ERROR,
                                (int16_t)beginState, false);
        failed = true;
        return;
    }

    if (applyBenchFault(BenchFaultPoint::AFTER_RETUNE, nullptr, 0, aborted, failed) ||
        applyBenchFault(BenchFaultPoint::CAD_WAIT, nullptr, 0, aborted, failed)) {
        return;
    }

    if (!waitForDioUntil(DISCOVERY_CAD_TIMEOUT_MS, energyAbortPending)) {
        if (energyAbortPending()) {
            aborted = true;
            return;
        }
        enqueuePassBObservation(bin, combo, EnergyObservationResult::CAD_TIMEOUT, 0, false);
        return;
    }

    ulTaskNotifyTake(pdTRUE, 0);
    int16_t scanState;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            busMissCount++;
            failed = true;
            return;
        }
        scanState = (int16_t)radio.getChannelScanResult();
    }
    if (scanState != RADIOLIB_LORA_DETECTED && scanState != RADIOLIB_CHANNEL_FREE) {
        lastError = scanState;
        enqueuePassBObservation(bin, combo, EnergyObservationResult::RADIO_ERROR,
                                scanState, false);
        failed = true;
        return;
    }
    if (scanState != RADIOLIB_LORA_DETECTED) {
        enqueuePassBObservation(bin, combo, EnergyObservationResult::CAD_FREE, scanState,
                                false);
        return;
    }

    // Detected: bounded receive-on-hit, same reasoning as Probe's own
    // -- a CAD hit is not a packet yet.
    int receiveState;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            busMissCount++;
            failed = true;
            return;
        }
        receiveState = radio.startReceive();
    }
    if (receiveState != RADIOLIB_ERR_NONE) {
        lastError = receiveState;
        enqueuePassBObservation(bin, combo, EnergyObservationResult::RADIO_ERROR,
                                (int16_t)receiveState, false);
        failed = true;
        return;
    }

    if (applyBenchFault(BenchFaultPoint::RX_WAIT, nullptr, 0, aborted, failed)) {
        return;
    }
    bool gotPacket = false;
    if (waitForDioUntil(DISCOVERY_RX_WINDOW_MS, energyAbortPending)) {
        ulTaskNotifyTake(pdTRUE, 0);
        uint8_t buf[DETECTION_RAW_MAX_LEN];
        Detection detection;
        bool readFailed;
        bool haveDetection = false;
        {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) {
                const ChannelParams passBChannel = {freq, combo.sf, combo.bw_khz,
                                                    combo.cr_denom, combo.sync_word};
                haveDetection = readDetectionLocked(passBChannel, activeProfile, buf,
                                                    sizeof(buf), detection, readFailed);
                if (readFailed) crcErrorCount++;
            } else {
                busMissCount++;
            }
        }
        if (haveDetection) {
            // Never Meshtastic/MeshCore attribution: an off-grid hit is
            // "unknown LoRa candidate" per docs/DESIGN.md §7.2, regardless
            // of activeProfile (always RETICULUM/GENERAL_EXPLORATION
            // here) -- see detection.h's detectionClassification().
            detection.off_grid = true;
            enqueueDetection(detection);
            enqueueNodeIdentity(detection);
            passBDetectionCount++;
            gotPacket = true;
        }
    } else if (energyAbortPending()) {
        aborted = true;
    }
    enqueuePassBObservation(bin, combo, EnergyObservationResult::CAD_DETECTED, scanState,
                            gotPacket);
}

void passBCadAtBin(uint16_t bin, float freq, bool &aborted, bool &failed) {
    for (uint8_t c = 0; c < PASS_B_SF_BW_CANDIDATE_COUNT && !aborted && !failed; c++) {
        if (energyAbortPending()) {
            aborted = true;
            break;
        }
        passBCadOneCombo(bin, freq, PASS_B_SF_BW_CANDIDATES[c], aborted, failed);
    }
}

// Bench-only (research/phase9-sweep-pass-b-design.md's false-positive-vs-SF
// bench matrix): runs passBCadOneCombo() for exactly one combo at a fixed
// test frequency, independent of any real Pass-A peak -- production Pass B
// only ever runs at a bin Pass A already flagged, so a quiet-room false-
// positive baseline can't be measured through a real ENERGY_SWEEP at all
// (zero peaks means Pass B never runs). Logs through the same
// enqueuePassBObservation()/energy.csv path as production Pass B, so the
// same per-attempt CAD_FREE/CAD_DETECTED/CAD_TIMEOUT analysis applies.
void performBenchPassBCadTrigger(uint8_t comboIndex) {
    benchPassBCadActive = true;
    if (comboIndex < PASS_B_SF_BW_CANDIDATE_COUNT) {
        const ChannelParams homeChannel = activeChannel;
        const MissionProfile homeProfile = activeProfile;
        bool aborted = false;
        bool failed = false;
        const uint16_t bin = energyBinIndexForFrequencyMhz(
            BENCH_PASS_B_CAD_TEST_FREQ_MHZ, energySweepBandForRegion(activeEnergySweepRegion),
            ENERGY_SWEEP_DEFAULT_STEP);
        passBCadOneCombo(bin, BENCH_PASS_B_CAD_TEST_FREQ_MHZ, PASS_B_SF_BW_CANDIDATES[comboIndex],
                         aborted, failed);
        restoreHomeListen(homeChannel, homeProfile);
    }
    benchPassBCadActive = false;
}

// Bench-only (docs/STATUS.md's 923MHz-edge injected-carrier
// characterization): parks the radio at freq_khz and samples RSSI
// continuously for BENCH_RSSI_WINDOW_SAMPLE_COUNT samples, tracking the
// running max/avg via the same streaming-stats helper Pass A uses
// (energy_observation.h) -- no raw sample history kept. A full Sweep's
// per-bin dwell is too brief to reliably coincide with an independently-
// timed transmitter's short burst; holding still for ~2s fixes that.
void performBenchRssiWindow(uint32_t freq_khz) {
    benchRssiWindowActive = true;
    benchRssiWindowHaveResult = false;
    const ChannelParams homeChannel = activeChannel;
    const MissionProfile homeProfile = activeProfile;
    const float freq = (float)freq_khz / 1000.0f;

    bool tuned = false;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            busMissCount++;
        } else {
            const int beginState = radio.begin(freq, homeChannel.bw_khz, homeChannel.sf,
                                                homeChannel.cr_denom, homeChannel.sync_word);
            if (beginState == RADIOLIB_ERR_NONE) {
                lastError = radio.startReceive();
                tuned = (lastError == RADIOLIB_ERR_NONE);
            } else {
                lastError = beginState;
            }
        }
    }

    if (tuned) {
        EnergyBinStats stats;
        for (uint16_t s = 0; s < BENCH_RSSI_WINDOW_SAMPLE_COUNT; s++) {
            {
                SpiBusLock lock(BUS_WAIT);
                if (lock.held()) {
                    const float rssi = radio.getRSSI(false);
                    energyBinStatsAddSample(stats, energyRssiDbmToFixed(rssi));
                } else {
                    busMissCount++;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(BENCH_RSSI_WINDOW_SAMPLE_INTERVAL_MS));
        }
        benchRssiWindowMaxDbmX10 = stats.rssi_peak_dbm_x10;
        benchRssiWindowAvgDbmX10 = stats.rssi_avg_dbm_x10;
        benchRssiWindowSampleCount = stats.sample_count;
        benchRssiWindowHaveResult = (stats.sample_count > 0);
    }

    restoreHomeListen(homeChannel, homeProfile);
    benchRssiWindowActive = false;
}

// docs/DESIGN.md §7.2's two-pass acquisition: Pass A is the bounded RSSI sweep
// across every frequency bin below, threshold-filtered peaks logged to
// energy.csv; Pass B (passBCadAtBin() above) runs inline the moment Pass A
// flags a bin as a peak, up to PASS_B_MAX_PEAKS_PER_SWEEP peaks per run.
// Home is restored on every exit path either way.
void performEnergySweep() {
    if (energyActive || discoveryActive || cellActive || scopeActive) return;

    energyActive = true;
    energyState = EnergySweepState::RUNNING;
    energyCancelRequested = false;
    const uint32_t awayStarted = millis();
    const ChannelParams homeChannel = activeChannel;
    const MissionProfile homeProfile = activeProfile;
    const EnergySweepBand band = energySweepBandForRegion(activeEnergySweepRegion);
    const uint16_t totalBins = energyBinCount(band, ENERGY_SWEEP_DEFAULT_STEP);
    energyBinIndex = 0;
    energyTotalBins = totalBins;
    energyPeakCount = 0;
    for (size_t i = 0; i < sizeof(energyPeakBinMask); i++) energyPeakBinMask[i] = 0;
    energyStrongestValid = false;
    benchSweepFloorReset();
    bool aborted = false;
    bool failed = false;
    // Rolling noise floor (energy_observation.h): seeded from bin 0's own
    // average rather than an arbitrary constant, since there is no prior
    // floor to compare against yet. Bin 0 is never itself logged as a peak.
    int16_t noiseFloor = 0;
    bool haveFloor = false;

    // Pass B (research/phase9-sweep-pass-b-design.md) only gets CAD time at
    // the first PASS_B_MAX_PEAKS_PER_SWEEP peaks Pass A finds, not every
    // peak -- run immediately as each one is discovered below (see
    // passBCadAtBin() above), not deferred until the whole sweep finishes.
    uint8_t passBPeaksThisSweep = 0;

    // A notification may be left by the home RX IRQ that woke the task to
    // service the Sweep request — same reasoning as Probe's own discard.
    ulTaskNotifyTake(pdTRUE, 0);

    for (uint16_t bin = 0; bin < totalBins; bin++) {
        if (energyAbortPending()) {
            aborted = true;
            break;
        }
        energyBinIndex = bin;

        if (applyBenchFault(BenchFaultPoint::BEFORE_RETUNE, nullptr, 0, aborted, failed)) {
            break;
        }

        const float freq = energyBinFrequencyMhz(bin, band, ENERGY_SWEEP_DEFAULT_STEP);
        int beginState;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                failed = true;
                break;
            }
            // Sweep is protocol-agnostic energy measurement, not a decode
            // attempt — reusing the home channel's own SF/BW/CR/sync keeps
            // this slice simple; a dedicated wide-BW scan config is a
            // future calibration decision, not a correctness requirement.
            beginState = radio.begin(freq, homeChannel.bw_khz, homeChannel.sf,
                                     homeChannel.cr_denom, homeChannel.sync_word);
            if (beginState == RADIOLIB_ERR_NONE) beginState = radio.startReceive();
        }
        if (beginState != RADIOLIB_ERR_NONE) {
            lastError = beginState;
            failed = true;
            break;
        }

        if (applyBenchFault(BenchFaultPoint::AFTER_RETUNE, nullptr, 0, aborted, failed)) {
            break;
        }

        EnergyBinStats stats;
        for (uint8_t s = 0; s < ENERGY_SAMPLES_PER_BIN; s++) {
            float rssi = 0.0f;
            bool gotSample = false;
            {
                SpiBusLock lock(BUS_WAIT);
                if (lock.held()) {
                    rssi = radio.getRSSI(false); // instantaneous, not last-packet
                    gotSample = true;
                } else {
                    busMissCount++;
                }
            }
            if (gotSample) {
                const int16_t fixed = energyRssiDbmToFixed(rssi);
                energyBinStatsAddSample(stats, fixed);
                if (haveFloor) {
                    energyBinStatsNoteOccupancy(
                        stats, energyExceedsFloor(fixed, noiseFloor, benchSweepMarginDbmX10()));
                }
            }
            if (s + 1 < ENERGY_SAMPLES_PER_BIN) vTaskDelay(pdMS_TO_TICKS(ENERGY_SAMPLE_INTERVAL_MS));
        }

        // Bench-only (bench_fault.h): keeps this bin's average even when it
        // isn't a peak, so a rolloff-characterization harness can read the
        // raw floor curve back after the sweep -- energy.csv only ever
        // persists threshold-filtered peaks. No-op on production firmware.
        benchSweepFloorRecord(bin, stats.rssi_avg_dbm_x10);

        if (!haveFloor) {
            noiseFloor = stats.rssi_avg_dbm_x10;
            haveFloor = true;
        } else {
            // Decide against the floor as it stood *before* this bin, then
            // fold this bin's average in — so a strong bin can't drag its
            // own floor upward and mask itself (energy_observation.h's own
            // noted concern). benchSweepMarginDbmX10() is the calibrated
            // production default (energy_observation.h) unless the
            // cardputer-adv-bench image has an operator-armed
            // BENCH_SWEEP_MARGIN override active.
            if (energyBinIsPeak(stats, noiseFloor, benchSweepMarginDbmX10())) {
                energyPeakBinMask[bin / 8] |= (uint8_t)(1U << (bin % 8));
                if (!energyStrongestValid || stats.rssi_peak_dbm_x10 > energyStrongestRssiDbmX10) {
                    energyStrongestFreqMhz = energyBinFrequencyMhz(bin, band, ENERGY_SWEEP_DEFAULT_STEP);
                    energyStrongestRssiDbmX10 = stats.rssi_peak_dbm_x10;
                    energyStrongestValid = true;
                }
                enqueueEnergyObservation(bin, homeChannel, stats);

                // Pass B, immediately -- not deferred (see
                // passBCadAtBin()'s own comment above for why). Bounded to
                // the first PASS_B_MAX_PEAKS_PER_SWEEP peaks this sweep;
                // later peaks still get logged as ENERGY_PEAK by Pass A
                // above, just no CAD time.
                if (passBPeaksThisSweep < PASS_B_MAX_PEAKS_PER_SWEEP) {
                    passBPeaksThisSweep++;
                    passBCadAtBin(bin, freq, aborted, failed);
                }
            }
            noiseFloor = energyNoiseFloorUpdate(noiseFloor, stats.rssi_avg_dbm_x10);
        }
        if (aborted || failed) break;
    }

    const bool requestPending =
        (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) ||
        (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0);
    ulTaskNotifyTake(pdTRUE, 0);
    applyBenchFault(BenchFaultPoint::HOME_RESTORE_BEFORE, nullptr, 0, aborted, failed);
    const bool restored = restoreHomeListen(homeChannel, homeProfile);
    applyBenchFault(BenchFaultPoint::HOME_RESTORE_AFTER, nullptr, 0, aborted, failed);
    if (requestPending && radioTaskHandle != nullptr) xTaskNotifyGive(radioTaskHandle);
    if (!restored) failed = true;

    // Snapshot before energySweepCount++ becomes visible to logger_task
    // (Core 0) -- see energyPeakBinMaskAtComplete's own comment above for
    // why this can't just be the live mask.
    for (size_t i = 0; i < sizeof(energyPeakBinMask); i++) {
        energyPeakBinMaskAtComplete[i] = energyPeakBinMask[i];
    }
    energySweepCount++;
    if (aborted || energyCancelRequested) energyCancelCount++;
    if (failed) energyFailureCount++;
    if (restored) energyRecoveryCount++;
    energyLastAwayMs = millis() - awayStarted;
    energyState = failed ? EnergySweepState::FAILED
                         : (aborted || energyCancelRequested ? EnergySweepState::CANCELLED
                                                              : EnergySweepState::COMPLETE);
    energyCancelRequested = false;
    energyActive = false;
}

// Isolated from performEnergySweep() on purpose (see radio_task.h's
// radioRequestCellSweep() comment): no shared noise-floor state, no CAD, no
// packet read attempt. Same bus-lock/abort-pending/home-restore-on-every-
// exit-path shape as performEnergySweep(), scoped to cell_plan.h's 101-bin
// 869-894MHz band instead of the full front end.
void performCellSweep() {
    if (cellActive || discoveryActive || energyActive || scopeActive) return;

    cellActive = true;
    cellState = CellSweepState::RUNNING;
    cellCancelRequested = false;
    const uint32_t awayStarted = millis();
    const ChannelParams homeChannel = activeChannel;
    const MissionProfile homeProfile = activeProfile;
    const uint16_t totalBins = cellBinCount();
    cellBinIndexState = 0;
    cellTotalBins = totalBins;
    cellStrongestValid = false;
    bool aborted = false;
    bool failed = false;

    // A notification may be left by the home RX IRQ that woke the task to
    // service the Cell request — same reasoning as Probe/Sweep's own
    // discard.
    ulTaskNotifyTake(pdTRUE, 0);

    for (uint16_t bin = 0; bin < totalBins; bin++) {
        if (cellAbortPending()) {
            aborted = true;
            break;
        }
        cellBinIndexState = bin;

        const float freq = cellBinFrequencyMhz(bin);
        int beginState;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                failed = true;
                break;
            }
            // RSSI-only measurement, not a decode attempt (cell_plan.h's
            // file header) — reusing the home channel's own SF/BW/CR/sync
            // keeps this simple; the configured receive bandwidth is
            // logged per-observation (rx_bw_khz) since it's a real
            // measurement condition, not a claim about the carrier itself.
            beginState = radio.begin(freq, homeChannel.bw_khz, homeChannel.sf,
                                     homeChannel.cr_denom, homeChannel.sync_word);
            if (beginState == RADIOLIB_ERR_NONE) beginState = radio.startReceive();
        }
        if (beginState != RADIOLIB_ERR_NONE) {
            lastError = beginState;
            EnergyBinStats emptyStats;
            enqueueCellObservation(bin, homeChannel, emptyStats, CellObservationResult::RADIO_ERROR,
                                   (int16_t)beginState);
            failed = true;
            break;
        }

        EnergyBinStats stats;
        for (uint8_t s = 0; s < ENERGY_SAMPLES_PER_BIN; s++) {
            float rssi = 0.0f;
            bool gotSample = false;
            {
                SpiBusLock lock(BUS_WAIT);
                if (lock.held()) {
                    rssi = radio.getRSSI(false); // instantaneous, not last-packet
                    gotSample = true;
                } else {
                    busMissCount++;
                }
            }
            if (gotSample) energyBinStatsAddSample(stats, energyRssiDbmToFixed(rssi));
            if (s + 1 < ENERGY_SAMPLES_PER_BIN) vTaskDelay(pdMS_TO_TICKS(ENERGY_SAMPLE_INTERVAL_MS));
        }

        enqueueCellObservation(bin, homeChannel, stats,
                               stats.sample_count > 0 ? CellObservationResult::MEASURED
                                                       : CellObservationResult::RADIO_ERROR,
                               0);
        if (aborted || failed) break;
    }

    const bool requestPending =
        (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) ||
        (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0);
    ulTaskNotifyTake(pdTRUE, 0);
    const bool restored = restoreHomeListen(homeChannel, homeProfile);
    if (requestPending && radioTaskHandle != nullptr) xTaskNotifyGive(radioTaskHandle);
    if (!restored) failed = true;

    cellSweepCount++;
    if (aborted || cellCancelRequested) cellCancelCount++;
    if (failed) cellFailureCount++;
    if (restored) cellRecoveryCount++;
    cellLastAwayMs = millis() - awayStarted;
    cellState = failed ? CellSweepState::FAILED
                        : (aborted || cellCancelRequested ? CellSweepState::CANCELLED
                                                           : CellSweepState::COMPLETE);
    cellCancelRequested = false;
    cellActive = false;
}

// Field Analyzer's Scope view (docs/research/LoRaTrace-Phases-7-10-
// Design.md §8.2/§8.4): parks the radio at freq_khz, same way
// performBenchRssiWindow() does, but pushes every sample into
// sharedScopeTrace under scopeTraceMutex instead of only keeping a running
// aggregate — the Scope view needs the actual time series, not a summary.
// Same bus-lock/abort-pending/home-restore-on-every-exit-path shape as
// performCellSweep() above.
void performScopeAcquire(uint32_t freq_khz) {
    if (scopeActive || discoveryActive || energyActive || cellActive) return;

    scopeActive = true;
    scopeState = ScopeAcquireState::RUNNING;
    scopeCancelRequested = false;
    const uint32_t awayStarted = millis();
    const ChannelParams homeChannel = activeChannel;
    const MissionProfile homeProfile = activeProfile;
    const float freq = (float)freq_khz / 1000.0f;
    bool failed = false;

    // A notification may be left by the home RX IRQ that woke the task to
    // service the Scope request — same reasoning as Probe/Sweep/Cell's own
    // discard.
    ulTaskNotifyTake(pdTRUE, 0);

    if (xSemaphoreTake(scopeTraceMutex, portMAX_DELAY) == pdTRUE) {
        scopeTraceReset(sharedScopeTrace, freq, (uint16_t)SCOPE_SAMPLE_INTERVAL_MS, awayStarted);
        xSemaphoreGive(scopeTraceMutex);
    }

    bool tuned = false;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            busMissCount++;
            failed = true;
        } else {
            const int beginState = radio.begin(freq, homeChannel.bw_khz, homeChannel.sf,
                                                homeChannel.cr_denom, homeChannel.sync_word);
            if (beginState == RADIOLIB_ERR_NONE) {
                lastError = radio.startReceive();
                tuned = (lastError == RADIOLIB_ERR_NONE);
                failed = !tuned;
            } else {
                lastError = beginState;
                failed = true;
            }
        }
    }

    bool aborted = false;
    if (tuned) {
        for (uint16_t s = 0; s < SCOPE_MAX_SAMPLES; s++) {
            if (scopeAbortPending()) {
                aborted = true;
                break;
            }
            {
                SpiBusLock lock(BUS_WAIT);
                if (lock.held()) {
                    const float rssi = radio.getRSSI(false); // instantaneous, not last-packet
                    if (xSemaphoreTake(scopeTraceMutex, BUS_WAIT) == pdTRUE) {
                        scopeTracePush(sharedScopeTrace, rssi);
                        xSemaphoreGive(scopeTraceMutex);
                    }
                } else {
                    busMissCount++;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(SCOPE_SAMPLE_INTERVAL_MS));
        }
    }

    const bool requestPending =
        (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) ||
        (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0);
    ulTaskNotifyTake(pdTRUE, 0);
    const bool restored = restoreHomeListen(homeChannel, homeProfile);
    if (requestPending && radioTaskHandle != nullptr) xTaskNotifyGive(radioTaskHandle);
    if (!restored) failed = true;

    scopeAcquireCount++;
    if (aborted || scopeCancelRequested) scopeCancelCount++;
    if (failed) scopeFailureCount++;
    if (restored) scopeRecoveryCount++;
    scopeLastAwayMs = millis() - awayStarted;
    scopeState = failed ? ScopeAcquireState::FAILED
                        : (aborted || scopeCancelRequested ? ScopeAcquireState::CANCELLED
                                                            : ScopeAcquireState::COMPLETE);
    scopeCancelRequested = false;
    scopeActive = false;
}

void radioTask(void *) {
    memoryStatsRegisterCurrentTask(MemoryTask::RADIO);
    uint8_t buf[DETECTION_RAW_MAX_LEN];
    bool paused = false;

    for (;;) {
        // Block until DIO1 says a packet landed. The timeout is a liveness
        // safety net, not an expected path — re-checks and re-arms if an
        // interrupt is ever missed. Skipped while paused: re-arming here
        // would silently undo radio.sleep() every 5s, and a sleeping
        // SX1262 can't miss a DIO1 it can't fire.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
            if (!paused) {
                SpiBusLock lock(BUS_WAIT);
                if (lock.held()) radio.startReceive();
            }
            continue;
        }

        // The same notification wakes this task for three reasons: a real
        // DIO1 packet IRQ, a profile-switch request, or a Trace
        // pause/resume request. Check both mailboxes first (non-blocking),
        // so a genuine packet arriving at the same instant is never held
        // up. Nothing queued in either means this really was a packet.
        PendingSwitch swreq;
        if (profileSwitchQueue != nullptr && xQueueReceive(profileSwitchQueue, &swreq, 0) == pdTRUE) {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) {
                // Reconfiguring the modem abandons anything mid-flight on
                // the old channel — accepted cost of docs/DESIGN.md §5's
                // "mutually exclusive": a switch means the operator no
                // longer wants the old profile.
                lastError = radio.begin(swreq.channel.freq_mhz, swreq.channel.bw_khz,
                                        swreq.channel.sf, swreq.channel.cr_denom,
                                        swreq.channel.sync_word);
                if (lastError == RADIOLIB_ERR_NONE) {
                    activeChannel = swreq.channel;
                    activeProfile = swreq.profile;
                }
                // A profile switch always means "go back to listening" —
                // even mid-pause, picking a different protocol is an
                // active choice that should be heard, not swallowed.
                paused = false;
                tracePaused = false;
                radio.startReceive();
            }
            continue;
        }

        bool pauseReq;
        if (pauseQueue != nullptr && xQueueReceive(pauseQueue, &pauseReq, 0) == pdTRUE) {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) {
                if (pauseReq) {
                    radio.sleep(true); // warm sleep — retains config, cheap to resume
                } else {
                    radio.startReceive(); // wakes a warm-sleeping SX126x automatically
                }
                paused = pauseReq;
                tracePaused = pauseReq;
            }
            continue;
        }

        bool discoveryReq;
        if (discoveryQueue != nullptr && xQueueReceive(discoveryQueue, &discoveryReq, 0) == pdTRUE) {
            if (discoveryReq && !paused) performDiscoverySweep();
            continue;
        }

        bool energyReq;
        if (energySweepQueue != nullptr && xQueueReceive(energySweepQueue, &energyReq, 0) == pdTRUE) {
            if (energyReq && !paused) {
                // Repeat mode (energyRepeatActive): keep re-running the same
                // bounded sweep back-to-back after each COMPLETE, until
                // either the operator stops it (radioRequestEnergySweepRepeat()
                // clears the flag and requests cancellation of whatever's
                // in flight) or a lap doesn't finish clean (CANCELLED/
                // FAILED) or Trace gets paused out from under it. A plain
                // tap (energyRepeatActive already false) runs this loop
                // exactly once, same as before this feature existed.
                do {
                    performEnergySweep();
                    if (energyRepeatActive) energyRepeatCount++;
                } while (energyRepeatActive && energyState == EnergySweepState::COMPLETE && !paused);
                energyRepeatActive = false;
            }
            continue;
        }

        bool cellReq;
        if (cellSweepQueue != nullptr && xQueueReceive(cellSweepQueue, &cellReq, 0) == pdTRUE) {
            if (cellReq && !paused) {
                // Repeat mode (cellRepeatActive): same back-to-back-laps
                // shape as energyRepeatActive above — keep re-running the
                // bounded scan until the operator stops it, a lap doesn't
                // finish clean, or Trace gets paused out from under it. A
                // plain tap (cellRepeatActive already false) runs this loop
                // exactly once.
                do {
                    performCellSweep();
                    if (cellRepeatActive) cellRepeatCount++;
                } while (cellRepeatActive && cellState == CellSweepState::COMPLETE && !paused);
                cellRepeatActive = false;
            }
            continue;
        }

        uint32_t scopeReqFreqKhz;
        if (scopeAcquireQueue != nullptr &&
            xQueueReceive(scopeAcquireQueue, &scopeReqFreqKhz, 0) == pdTRUE) {
            // No repeat mode (design doc doesn't call for one, and operator
            // decision on Cell/Sweep's own repeat was "Repeat only on the
            // Sweeps") — a UI wanting a continuously-refreshing Scope just
            // re-requests after each COMPLETE (ui_task.cpp, Stage 3).
            if (!paused) performScopeAcquire(scopeReqFreqKhz);
            continue;
        }

        uint8_t benchPassBCadComboIndex;
        if (benchPassBCadQueue != nullptr &&
            xQueueReceive(benchPassBCadQueue, &benchPassBCadComboIndex, 0) == pdTRUE) {
            if (!paused) performBenchPassBCadTrigger(benchPassBCadComboIndex);
            continue;
        }

        uint32_t benchRssiWindowFreqKhz;
        if (benchRssiWindowQueue != nullptr &&
            xQueueReceive(benchRssiWindowQueue, &benchRssiWindowFreqKhz, 0) == pdTRUE) {
            if (!paused) performBenchRssiWindow(benchRssiWindowFreqKhz);
            continue;
        }

        if (paused) {
            // Nothing else to do while asleep — DIO1 can't fire, and the
            // liveness branch above already skips re-arming.
            continue;
        }

        Detection det = {};
        bool haveDetection = false;
        bool readFailed = false;

        {
            // Everything touching the SX1262 happens inside this one short
            // critical section. Order matters: read the stats, then
            // re-arm RX *before* anything slow — the chip is deaf between
            // DIO1 firing and startReceive().
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                continue;
            }

            haveDetection = readDetectionLocked(activeChannel, activeProfile, buf, sizeof(buf),
                                                det, readFailed);
            if (readFailed) crcErrorCount++;
        } // bus released here, before any queue work

        if (haveDetection) {
            enqueueDetection(det);
            enqueueNodeIdentity(det);
        }
    }
}

} // namespace

bool radioTaskStart(const ChannelParams &channel, MissionProfile profile,
                    const ProfileOverrides &overrides, QueueHandle_t queue,
                    QueueHandle_t scanQueue, QueueHandle_t energyQueue,
                    QueueHandle_t nodesQueue, QueueHandle_t cellQueue) {
    activeChannel = channel;
    activeProfile = profile;
    activeOverrides = overrides;
    detectionQueue = queue;
    scanObservationQueue = scanQueue;
    energyObservationQueue = energyQueue;
    identityQueue = nodesQueue;
    cellObservationQueue = cellQueue;

    // Depth-1 mailbox for radioRequestProfileSwitch(). Created here, not
    // lazily, so a switch request right after boot can't race a
    // not-yet-existent queue.
    profileSwitchQueue = xQueueCreate(1, sizeof(PendingSwitch));
    if (profileSwitchQueue == nullptr) return false;

    pauseQueue = xQueueCreate(1, sizeof(bool));
    if (pauseQueue == nullptr) return false;

    discoveryQueue = xQueueCreate(1, sizeof(bool));
    if (discoveryQueue == nullptr) return false;

    energySweepQueue = xQueueCreate(1, sizeof(bool));
    if (energySweepQueue == nullptr) return false;

    cellSweepQueue = xQueueCreate(1, sizeof(bool));
    if (cellSweepQueue == nullptr) return false;

    scopeAcquireQueue = xQueueCreate(1, sizeof(uint32_t));
    if (scopeAcquireQueue == nullptr) return false;

    if (scopeTraceMutex == nullptr) {
        scopeTraceMutex = xSemaphoreCreateMutex();
        if (scopeTraceMutex == nullptr) return false;
    }

    benchPassBCadQueue = xQueueCreate(1, sizeof(uint8_t));
    if (benchPassBCadQueue == nullptr) return false;

    benchRssiWindowQueue = xQueueCreate(1, sizeof(uint32_t));
    if (benchRssiWindowQueue == nullptr) return false;

    {
        SpiBusLock lock(portMAX_DELAY);
        if (!lock.held()) return false;

        lastError = radio.begin(channel.freq_mhz, channel.bw_khz, channel.sf, channel.cr_denom,
                                channel.sync_word);
        if (lastError != RADIOLIB_ERR_NONE) return false;
    }

    // Create the task before wiring the ISR: onDio1Action dereferences
    // radioTaskHandle, so this ordering makes that dependency explicit.
    //
    // 6144, not 4096: an 8-hour endurance soak (2026-09-03, docs/STATUS.md)
    // pulled real session.csv data showing radio_stack_free's lifetime-
    // minimum watermark settling at 820B free (20.0%) within the first two
    // hours and never dropping further -- no leak, but below this
    // project's own documented margin rule (25% or 1KB, whichever is
    // larger; 4096 needed >=1024B free and only had 820B). Bumped
    // proportionate to how logger_task's own near-identical story (sized
    // by inspection, never load-tested, found undersized only once a real
    // soak stress-tested it) got fixed just before this in the same soak
    // cycle -- radio_task is the single most critical task here (owns the
    // SX1262, must never block), not one to leave at a margin that's
    // already below the house rule just because it hadn't overflowed yet.
    BaseType_t ok =
        xTaskCreatePinnedToCore(radioTask, "radio", 6144, nullptr, 3, &radioTaskHandle, 1);
    if (ok != pdPASS) return false;

    SpiBusLock lock(portMAX_DELAY);
    if (!lock.held()) return false;
    radio.setDio1Action(onDio1Action);
    lastError = radio.startReceive();
    return lastError == RADIOLIB_ERR_NONE;
}

int radioLastError() {
    return lastError;
}

uint32_t radioPacketCount() {
    return packetCount;
}
uint32_t radioCrcErrorCount() {
    return crcErrorCount;
}
uint32_t radioQueueDropCount() {
    return queueDropCount;
}
void radioIdentityCaptureSetEnabled(bool enabled) {
    identityCaptureEnabled = enabled;
}
bool radioIdentityCaptureIsEnabled() {
    return identityCaptureEnabled;
}
uint32_t radioIdentityDecodeCount() {
    return identityDecodeCount;
}
uint32_t radioIdentityDropCount() {
    return identityDropCount;
}
uint32_t radioBusMissCount() {
    return busMissCount;
}
uint32_t radioScanObservationDropCount() {
    return scanObservationDropCount;
}
uint32_t radioScanObservationCount() {
    return scanObservationCount;
}
uint32_t radioDiscoverySweepCount() {
    return discoverySweepCount;
}
uint32_t radioDiscoveryCancelCount() {
    return discoveryCancelCount;
}
uint32_t radioDiscoveryTimeoutCount() {
    return discoveryTimeoutCount;
}
uint32_t radioDiscoveryFailureCount() {
    return discoveryFailureCount;
}
uint32_t radioDiscoveryRecoveryCount() {
    return discoveryRecoveryCount;
}
uint32_t radioDiscoveryLastAwayMs() {
    return discoveryLastAwayMs;
}

uint8_t radioDiscoveryCandidateIndex() {
    return discoveryCandidateIndex;
}

uint8_t radioDiscoveryCandidateCount() {
    return discoveryCandidateCount;
}
DiscoverySweepState radioDiscoverySweepState() {
    return discoveryState;
}
uint16_t radioDiscoveryCadFreeCount() {
    return discoveryCadFreeCount;
}
uint16_t radioDiscoveryCadDetectedCount() {
    return discoveryCadDetectedCount;
}
uint16_t radioDiscoveryCadDetectedMask() {
    return discoveryCadDetectedMask;
}
uint16_t radioDiscoveryCadTimeoutCount() {
    return discoveryCadTimeoutCount;
}
uint16_t radioDiscoveryErrorCount() {
    return discoveryErrorCount;
}

ChannelParams radioActiveChannel() {
    return activeChannel; // small POD struct, cheap to return by value
}

MissionProfile radioActiveProfile() {
    return activeProfile; // same small-POD, no-lock convention as above
}

ProfileOverrides radioActiveOverrides() {
    return activeOverrides; // same small-POD, no-lock convention as above
}

bool radioRequestProfileSwitch(MissionProfile profile) {
    if (profileSwitchQueue == nullptr || radioTaskHandle == nullptr) return false;
    // resolvedChannelForProfile(), not channelParamsForProfile() directly —
    // otherwise switching to a profile would always use its hardcoded
    // table, silently dropping any loaded SD/web override.
    PendingSwitch req{profile, resolvedChannelForProfile(activeOverrides, profile)};
    xQueueOverwrite(profileSwitchQueue, &req);
    // Wakes the radio task immediately even if parked in the 5s liveness
    // wait — otherwise the switch could sit in the mailbox that long.
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioRequestTracePause(bool paused) {
    if (pauseQueue == nullptr || radioTaskHandle == nullptr) return false;
    xQueueOverwrite(pauseQueue, &paused);
    xTaskNotifyGive(radioTaskHandle); // same immediate-wake reason as above
    return true;
}

bool radioIsTracePaused() {
    return tracePaused;
}

bool radioRequestDiscoverySweep() {
    if (radioTaskHandle == nullptr || discoveryQueue == nullptr) return false;
    if (discoveryActive) {
        discoveryCancelRequested = true;
        xTaskNotifyGive(radioTaskHandle);
        return true;
    }
    // Mutually exclusive with Sweep, Cell, and Scope (docs/DESIGN.md §5) —
    // none can preempt another; see radioRequestEnergySweep()'s/
    // radioRequestCellSweep()'s/radioRequestScopeAcquire()'s matching
    // guards.
    if (tracePaused || energyActive || cellActive || scopeActive) return false;
    const bool start = true;
    xQueueOverwrite(discoveryQueue, &start);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioDiscoverySweepIsActive() {
    return discoveryActive;
}

bool radioRequestEnergySweep() {
    if (radioTaskHandle == nullptr || energySweepQueue == nullptr) return false;
    if (energyActive) {
        energyCancelRequested = true;
        xTaskNotifyGive(radioTaskHandle);
        return true;
    }
    if (tracePaused || discoveryActive || cellActive || scopeActive) return false;
    const bool start = true;
    xQueueOverwrite(energySweepQueue, &start);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioEnergySweepIsActive() {
    return energyActive;
}

// R key (operator request, 2026-08-29; moved off a Ctrl+S chord to its own
// dedicated key 2026-08-30 after real hardware testing showed the TCA8418
// can drop Ctrl's release event — see keyboard.h): starts/stops a chain of
// back-to-back Sweeps instead of one bounded run. Shares the exact same
// start path as a plain tap (radioRequestEnergySweep() above) — this only
// sets the flag the main loop's repeat do-while (above) checks after each
// COMPLETE, and only differs from a plain tap in the stop case, where it
// also clears energyRepeatActive so the loop doesn't start another lap.
bool radioRequestEnergySweepRepeat() {
    if (radioTaskHandle == nullptr || energySweepQueue == nullptr) return false;
    if (energyRepeatActive) {
        energyRepeatActive = false;
        if (energyActive) {
            energyCancelRequested = true;
            xTaskNotifyGive(radioTaskHandle);
        }
        return true;
    }
    if (energyActive || tracePaused || discoveryActive || cellActive || scopeActive) return false;
    energyRepeatActive = true;
    energyRepeatCount = 0;
    const bool start = true;
    xQueueOverwrite(energySweepQueue, &start);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioEnergySweepRepeatIsActive() {
    return energyRepeatActive;
}

uint32_t radioEnergySweepRepeatCount() {
    return energyRepeatCount;
}

void radioSetEnergySweepRegion(Region region) {
    activeEnergySweepRegion = region;
}

Region radioEnergySweepRegion() {
    return activeEnergySweepRegion;
}

bool radioRequestBenchPassBCadTrigger(uint8_t comboIndex) {
    if (!benchPassBCadTriggerAllowed()) return false;
    if (radioTaskHandle == nullptr || benchPassBCadQueue == nullptr) return false;
    if (comboIndex >= PASS_B_SF_BW_CANDIDATE_COUNT) return false;
    if (benchPassBCadActive || energyActive || discoveryActive || tracePaused || scopeActive) {
        return false;
    }
    xQueueOverwrite(benchPassBCadQueue, &comboIndex);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioBenchPassBCadIsActive() {
    return benchPassBCadActive;
}

bool radioRequestBenchRssiWindow(uint32_t freq_khz) {
    if (!benchRssiWindowTriggerAllowed()) return false;
    if (radioTaskHandle == nullptr || benchRssiWindowQueue == nullptr) return false;
    if (freq_khz < 860000UL || freq_khz > 930000UL) return false;
    if (benchRssiWindowActive || benchPassBCadActive || energyActive || discoveryActive ||
        cellActive || tracePaused || scopeActive) {
        return false;
    }
    xQueueOverwrite(benchRssiWindowQueue, &freq_khz);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioBenchRssiWindowIsActive() {
    return benchRssiWindowActive;
}

bool radioBenchRssiWindowResult(int16_t &max_dbm_x10, int16_t &avg_dbm_x10, uint16_t &sample_count) {
    if (!benchRssiWindowTriggerAllowed()) return false;
    if (benchRssiWindowActive || !benchRssiWindowHaveResult) return false;
    max_dbm_x10 = benchRssiWindowMaxDbmX10;
    avg_dbm_x10 = benchRssiWindowAvgDbmX10;
    sample_count = benchRssiWindowSampleCount;
    return true;
}

uint16_t radioEnergyBinIndex() {
    return energyBinIndex;
}

uint16_t radioEnergyBinCount() {
    return energyTotalBins;
}

uint16_t radioEnergyPeakCount() {
    return energyPeakCount;
}

bool radioEnergyPeakBinSet(uint16_t bin) {
    if (bin >= sizeof(energyPeakBinMask) * 8) return false;
    return (energyPeakBinMask[bin / 8] & (uint8_t)(1U << (bin % 8))) != 0;
}

bool radioEnergyPeakBinSetAtLastComplete(uint16_t bin) {
    if (bin >= sizeof(energyPeakBinMaskAtComplete) * 8) return false;
    return (energyPeakBinMaskAtComplete[bin / 8] & (uint8_t)(1U << (bin % 8))) != 0;
}

EnergyStrongestPeak radioEnergyStrongestPeak() {
    EnergyStrongestPeak peak;
    peak.freq_mhz = energyStrongestFreqMhz;
    peak.rssi_peak_dbm_x10 = energyStrongestRssiDbmX10;
    peak.valid = energyStrongestValid;
    return peak;
}

EnergySweepState radioEnergySweepState() {
    return energyState;
}

uint32_t radioEnergyObservationCount() {
    return energyObservationCount;
}

uint32_t radioEnergyObservationDropCount() {
    return energyObservationDropCount;
}

uint32_t radioEnergySweepCount() {
    return energySweepCount;
}

uint32_t radioEnergyCancelCount() {
    return energyCancelCount;
}

uint32_t radioEnergyFailureCount() {
    return energyFailureCount;
}

uint32_t radioEnergyRecoveryCount() {
    return energyRecoveryCount;
}

uint32_t radioEnergyLastAwayMs() {
    return energyLastAwayMs;
}

uint32_t radioPassBAttemptCount() {
    return passBAttemptCount;
}

uint32_t radioPassBDetectionCount() {
    return passBDetectionCount;
}

bool radioRequestCellSweep() {
    if (radioTaskHandle == nullptr || cellSweepQueue == nullptr) return false;
    if (cellActive) {
        cellCancelRequested = true;
        xTaskNotifyGive(radioTaskHandle);
        return true;
    }
    // Mutually exclusive with Probe, Sweep, and Scope — same convention as
    // their own guards above.
    if (tracePaused || discoveryActive || energyActive || scopeActive) return false;
    const bool start = true;
    xQueueOverwrite(cellSweepQueue, &start);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioCellSweepIsActive() {
    return cellActive;
}

// R key, page-gated to the Cell card by ui_task.cpp: starts/stops a chain
// of back-to-back Cell scans instead of one bounded run. Exact mirror of
// radioRequestEnergySweepRepeat() above — shares the same start path as a
// plain tap (radioRequestCellSweep()), only differs in the stop case, where
// it also clears cellRepeatActive so the loop doesn't start another lap.
bool radioRequestCellSweepRepeat() {
    if (radioTaskHandle == nullptr || cellSweepQueue == nullptr) return false;
    if (cellRepeatActive) {
        cellRepeatActive = false;
        if (cellActive) {
            cellCancelRequested = true;
            xTaskNotifyGive(radioTaskHandle);
        }
        return true;
    }
    if (cellActive || tracePaused || discoveryActive || energyActive || scopeActive) return false;
    cellRepeatActive = true;
    cellRepeatCount = 0;
    const bool start = true;
    xQueueOverwrite(cellSweepQueue, &start);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioCellSweepRepeatIsActive() {
    return cellRepeatActive;
}

uint32_t radioCellSweepRepeatCount() {
    return cellRepeatCount;
}

CellSweepState radioCellSweepState() {
    return cellState;
}

uint16_t radioCellBinIndex() {
    return cellBinIndexState;
}

uint16_t radioCellBinCount() {
    return cellTotalBins;
}

CellStrongestSignal radioCellStrongestSignal() {
    CellStrongestSignal signal;
    signal.freq_mhz = cellStrongestFreqMhz;
    signal.rssi_peak_dbm_x10 = cellStrongestRssiDbmX10;
    signal.valid = cellStrongestValid;
    return signal;
}

uint32_t radioCellObservationCount() {
    return cellObservationCount;
}

uint32_t radioCellObservationDropCount() {
    return cellObservationDropCount;
}

uint32_t radioCellSweepCount() {
    return cellSweepCount;
}

uint32_t radioCellCancelCount() {
    return cellCancelCount;
}

uint32_t radioCellFailureCount() {
    return cellFailureCount;
}

uint32_t radioCellRecoveryCount() {
    return cellRecoveryCount;
}

uint32_t radioCellLastAwayMs() {
    return cellLastAwayMs;
}

bool radioRequestScopeAcquire(uint32_t freq_khz) {
    if (radioTaskHandle == nullptr || scopeAcquireQueue == nullptr) return false;
    if (scopeActive) {
        scopeCancelRequested = true;
        xTaskNotifyGive(radioTaskHandle);
        return true;
    }
    // Mutually exclusive with Probe, Sweep, and Cell — same convention as
    // their own guards above.
    if (tracePaused || discoveryActive || energyActive || cellActive) return false;
    xQueueOverwrite(scopeAcquireQueue, &freq_khz);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioScopeAcquireIsActive() {
    return scopeActive;
}

ScopeAcquireState radioScopeAcquireState() {
    return scopeState;
}

bool radioScopeTraceSnapshot(ScopeTrace &out, TickType_t timeout) {
    if (scopeTraceMutex == nullptr) return false;
    if (xSemaphoreTake(scopeTraceMutex, timeout) != pdTRUE) return false;
    out = sharedScopeTrace;
    xSemaphoreGive(scopeTraceMutex);
    return true;
}

uint32_t radioScopeAcquireCount() {
    return scopeAcquireCount;
}

uint32_t radioScopeCancelCount() {
    return scopeCancelCount;
}

uint32_t radioScopeFailureCount() {
    return scopeFailureCount;
}

uint32_t radioScopeRecoveryCount() {
    return scopeRecoveryCount;
}

uint32_t radioScopeLastAwayMs() {
    return scopeLastAwayMs;
}
