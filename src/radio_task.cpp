#include "radio_task.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/task.h>

#include "board_pins.h"
#include "bench_fault.h"
#include "discovery_plan.h"
#include "energy_plan.h"
#include "memory_stats.h"
#include "spi_bus.h"

namespace {

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, sharedSpi());

TaskHandle_t radioTaskHandle = nullptr;
QueueHandle_t detectionQueue = nullptr;
QueueHandle_t scanObservationQueue = nullptr;
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
volatile uint16_t energyBinIndex = 0;
volatile uint16_t energyTotalBins = 0;
volatile EnergySweepState energyState = EnergySweepState::IDLE;
volatile uint16_t energyPeakCount = 0;

int lastError = RADIOLIB_ERR_NONE;

volatile uint32_t packetCount = 0;
volatile uint32_t crcErrorCount = 0;
volatile uint32_t queueDropCount = 0;
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
    det.raw_len = (uint16_t)len;
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
// (DESIGN.md §8.1: "don't dump every sweep point, only peaks"). `wifi_on`
// is hardcoded false: a real WiFi-state getter is a later slice's concern
// (this task has no WiFi dependency today and shouldn't grow one just to
// answer this field).
void enqueueEnergyObservation(uint16_t binIndex, const ChannelParams &channel,
                              const EnergyBinStats &stats) {
    EnergyObservation observation;
    observation.rx_millis = millis();
    observation.freq_mhz = energyBinFrequencyMhz(binIndex, ENERGY_SWEEP_DEFAULT_STEP);
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

bool energyAbortPending() {
    if (energyCancelRequested) return true;
    if (profileSwitchQueue != nullptr && uxQueueMessagesWaiting(profileSwitchQueue) > 0) return true;
    if (pauseQueue != nullptr && uxQueueMessagesWaiting(pauseQueue) > 0) return true;
    return false;
}

bool waitForDioUntil(uint32_t timeoutMs) {
    const uint32_t started = millis();
    for (;;) {
        if (digitalRead(PIN_LORA_IRQ)) return true;
        if (discoveryAbortPending()) return false;
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
    if (discoveryActive) return;

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
        uint8_t buf[256];
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
        if (haveDetection) enqueueDetection(detection);
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

// Pass A only (DESIGN.md §7.2): bounded RSSI sweep across every frequency
// bin, threshold-filtered peaks logged, home restored on every exit path.
// Pass B (selective CAD at peaks) is a later slice — see energy_observation.h's
// EnergyObservationResult comment for why its enum has no CAD outcomes yet.
void performEnergySweep() {
    if (energyActive || discoveryActive) return;

    energyActive = true;
    energyState = EnergySweepState::RUNNING;
    energyCancelRequested = false;
    const uint32_t awayStarted = millis();
    const ChannelParams homeChannel = activeChannel;
    const MissionProfile homeProfile = activeProfile;
    const uint16_t totalBins = energyBinCount(ENERGY_SWEEP_DEFAULT_STEP);
    energyBinIndex = 0;
    energyTotalBins = totalBins;
    energyPeakCount = 0;
    bool aborted = false;
    bool failed = false;
    // Rolling noise floor (energy_observation.h): seeded from bin 0's own
    // average rather than an arbitrary constant, since there is no prior
    // floor to compare against yet. Bin 0 is never itself logged as a peak.
    int16_t noiseFloor = 0;
    bool haveFloor = false;

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

        const float freq = energyBinFrequencyMhz(bin, ENERGY_SWEEP_DEFAULT_STEP);
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

        if (!haveFloor) {
            noiseFloor = stats.rssi_avg_dbm_x10;
            haveFloor = true;
        } else {
            // Decide against the floor as it stood *before* this bin, then
            // fold this bin's average in — so a strong bin can't drag its
            // own floor upward and mask itself (energy_observation.h's own
            // noted concern). benchSweepMarginDbmX10() is the production
            // default (100 = 10.0dB) unless the cardputer-adv-bench image
            // has an operator-armed BENCH_SWEEP_MARGIN override active.
            if (energyBinIsPeak(stats, noiseFloor, benchSweepMarginDbmX10())) {
                enqueueEnergyObservation(bin, homeChannel, stats);
            }
            noiseFloor = energyNoiseFloorUpdate(noiseFloor, stats.rssi_avg_dbm_x10);
        }
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

void radioTask(void *) {
    memoryStatsRegisterCurrentTask(MemoryTask::RADIO);
    uint8_t buf[256];
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
                // the old channel — accepted cost of DESIGN.md §5's
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
            if (energyReq && !paused) performEnergySweep();
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

        if (haveDetection) enqueueDetection(det);
    }
}

} // namespace

bool radioTaskStart(const ChannelParams &channel, MissionProfile profile,
                    const ProfileOverrides &overrides, QueueHandle_t queue,
                    QueueHandle_t scanQueue, QueueHandle_t energyQueue) {
    activeChannel = channel;
    activeProfile = profile;
    activeOverrides = overrides;
    detectionQueue = queue;
    scanObservationQueue = scanQueue;
    energyObservationQueue = energyQueue;

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

    {
        SpiBusLock lock(portMAX_DELAY);
        if (!lock.held()) return false;

        lastError = radio.begin(channel.freq_mhz, channel.bw_khz, channel.sf, channel.cr_denom,
                                channel.sync_word);
        if (lastError != RADIOLIB_ERR_NONE) return false;
    }

    // Create the task before wiring the ISR: onDio1Action dereferences
    // radioTaskHandle, so this ordering makes that dependency explicit.
    BaseType_t ok =
        xTaskCreatePinnedToCore(radioTask, "radio", 4096, nullptr, 3, &radioTaskHandle, 1);
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
    // Mutually exclusive with Sweep (DESIGN.md §5) — neither can preempt
    // the other; see radioRequestEnergySweep()'s matching guard.
    if (tracePaused || energyActive) return false;
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
    if (tracePaused || discoveryActive) return false;
    const bool start = true;
    xQueueOverwrite(energySweepQueue, &start);
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioEnergySweepIsActive() {
    return energyActive;
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
