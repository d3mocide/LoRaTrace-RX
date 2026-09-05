#include "serial_control.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "logger_task.h"
#include "bench_fault.h"
#include "wifi_task.h"
#include "pass_b_plan.h"
#include "serial_control_protocol.h"
#include "radio_task.h"
#include "serial_lock.h"
#include "ui_task.h"
#include "version.h"

namespace {

volatile bool enabled = false;
// Native USB opening resets this ESP32-S3, so the explicit physical gate is
// persisted in NVS. It remains off until the operator enables it on-device.
// The NVS namespace/key strings stay "lowprofile"/"usb_enabled" (not renamed
// to match Serial Control) so a card already carrying this setting from
// before the rename keeps working — same host/device-compatibility reasoning
// as serial_control_protocol.h's wire opcode names.
constexpr char SERIAL_CONTROL_NVS_NAMESPACE[] = "lowprofile";
constexpr char SERIAL_CONTROL_NVS_KEY[] = "usb_enabled";
bool persistedStateLoaded = false;
char input[SERIAL_CONTROL_FRAME_MAX] = {};
size_t inputLength = 0;
uint16_t lastSequence = 0;
char lastResponse[SERIAL_CONTROL_FRAME_MAX] = {};
bool haveLastResponse = false;
SerialControlOpcode lastRequestOpcode = SerialControlOpcode::INVALID;
char lastRequestArgument[80] = {};

void clearInput() {
    inputLength = 0;
    input[0] = '\0';
}

void loadPersistedEnable() {
    if (persistedStateLoaded) return;
    Preferences preferences;
    if (preferences.begin(SERIAL_CONTROL_NVS_NAMESPACE, true)) {
        enabled = preferences.getBool(SERIAL_CONTROL_NVS_KEY, false);
        preferences.end();
    }
    persistedStateLoaded = true;
}

void sendFrame(uint16_t sequence, SerialControlOpcode opcode, const char *argument,
               bool cache = true) {
    char frame[SERIAL_CONTROL_FRAME_MAX] = {};
    if (serialControlFormatFrame(frame, sizeof(frame), sequence, opcode, argument) == 0) return;
    if (cache) {
        lastSequence = sequence;
        strncpy(lastResponse, frame, sizeof(lastResponse) - 1);
        lastResponse[sizeof(lastResponse) - 1] = '\0';
        haveLastResponse = true;
    }
    SerialLock lock(pdMS_TO_TICKS(30));
    if (lock.held()) serialPrintln(frame);
}

void sendStatus(uint16_t sequence) {
    const char *profile = radioActiveProfile() == MissionProfile::MESHCORE ? "MESHCORE" : "MESHTASTIC";
    const char *probe = "IDLE";
    switch (radioDiscoverySweepState()) {
        case DiscoverySweepState::RUNNING: probe = "RUNNING"; break;
        case DiscoverySweepState::COMPLETE: probe = "COMPLETE"; break;
        case DiscoverySweepState::CANCELLED: probe = "CANCELLED"; break;
        case DiscoverySweepState::FAILED: probe = "FAILED"; break;
        default: break;
    }
    const char *sweep = "IDLE";
    switch (radioEnergySweepState()) {
        case EnergySweepState::RUNNING: sweep = "RUNNING"; break;
        case EnergySweepState::COMPLETE: sweep = "COMPLETE"; break;
        case EnergySweepState::CANCELLED: sweep = "CANCELLED"; break;
        case EnergySweepState::FAILED: sweep = "FAILED"; break;
        default: break;
    }
    const ChannelParams channel = radioActiveChannel();
    // C is the last Probe's compact CAD result tuple: free,detected,timeout,error.
    // M is its fixed plan-index detection bitmask, which lets a harness tie
    // a known fixture pulse to its intended candidate rather than any ambient hit.
    // It is intentionally one bounded field so operator and harness STATUS
    // polling can measure CAD behavior without extracting probe.csv first.
    // W/WI/WN/WP are Sweep's equivalent compact summary: terminal state,
    // current/last bin index, total bin count, and peaks logged this run —
    // same "measure without extracting energy.csv first" reasoning as Probe's.
    // PBA/PBD are Pass B's own cumulative-since-boot counters (research/
    // phase9-sweep-pass-b-design.md) -- CAD attempts at Pass-A peaks and how
    // many promoted a real packet -- not reset per sweep the way WP is, same
    // "cumulative alongside per-run fields" convention R= already uses here.
    // BPC is the bench-only single-combo CAD trigger's own active flag, so
    // the false-positive-vs-SF bench matrix can poll for one request's
    // completion (1 -> 0) without racing unrelated PBA increments. RW is
    // the bench-only RSSI window's own active flag, same polling reason
    // (docs/STATUS.md's 923MHz-edge injected-carrier characterization).
    // WIFI mirrors wifiIsEnabled() (WIFI_SET's own request is async, so a
    // host script polls this to see the change actually take effect). EA
    // is the last Sweep's away-from-home duration in ms
    // (radioEnergyLastAwayMs(), already tracked internally, not
    // previously exposed) -- Phase 9's own "timing and home-away duration
    // are measured" exit criterion (ROADMAP.md) needs this outside the
    // on-device UI/energy.csv. RXP/RXC are Watch/Trace's own cumulative
    // counters (radioPacketCount()/radioCrcErrorCount(), both already
    // tracked internally, not previously exposed) -- added 2026-09-03
    // chasing a live "is Trace actually receiving this real traffic"
    // question that R= (Probe's recovery count, not a packet count
    // despite the letter) could not answer; distinct names on purpose so
    // a host script can't confuse them the way this session just did.
    // Sized from the protocol's own frame budget rather than by hand: this
    // buffer was 240 against a 230-byte budget, which silently dropped the
    // whole STATUS frame once the counters grew (serial_control_protocol.h).
    char argument[SERIAL_CONTROL_ARGUMENT_MAX + 1] = {};
    snprintf(argument, sizeof(argument),
             "P=%s;T=%u;B=%s;SD=%u;F=%lu;R=%lu;I=%u;N=%u;C=%u,%u,%u,%u;M=%04X;"
             "W=%s;WI=%u;WN=%u;WP=%u;PBA=%lu;PBD=%lu;BPC=%u;RW=%u;WIFI=%u;EA=%lu;"
             "RXP=%lu;RXC=%lu;FS=%u;FA=%lu;FO=%lu;FD=%lu;FW=%lu;FL=%lu",
             profile, radioIsTracePaused() ? 0U : 1U, probe, loggerSdReady() ? 1U : 0U,
             (unsigned long)(channel.freq_mhz * 1000.0f),
             (unsigned long)radioDiscoveryRecoveryCount(),
             (unsigned)radioDiscoveryCandidateIndex(),
             (unsigned)radioDiscoveryCandidateCount(),
             (unsigned)radioDiscoveryCadFreeCount(),
             (unsigned)radioDiscoveryCadDetectedCount(),
             (unsigned)radioDiscoveryCadTimeoutCount(),
             (unsigned)radioDiscoveryErrorCount(),
             (unsigned)radioDiscoveryCadDetectedMask(),
             sweep, (unsigned)radioEnergyBinIndex(), (unsigned)radioEnergyBinCount(),
             (unsigned)radioEnergyPeakCount(),
             (unsigned long)radioPassBAttemptCount(), (unsigned long)radioPassBDetectionCount(),
             radioBenchPassBCadIsActive() ? 1U : 0U, radioBenchRssiWindowIsActive() ? 1U : 0U,
             wifiIsEnabled() ? 1U : 0U, (unsigned long)radioEnergyLastAwayMs(),
             (unsigned long)radioPacketCount(), (unsigned long)radioCrcErrorCount(),
             (unsigned)radioFocusSurveyState(), (unsigned long)radioFocusLastAwayMs(),
             (unsigned long)radioFocusObservationCount(),
             (unsigned long)radioFocusObservationDropCount(),
             (unsigned long)loggerFocusRowsWritten(),
             (unsigned long)loggerFocusRowsDropped());
    sendFrame(sequence, SerialControlOpcode::STATUS, argument);
}

void handleFrame(const SerialControlFrame &frame) {
    if (haveLastResponse && frame.sequence == lastSequence &&
        frame.opcode == lastRequestOpcode && strcmp(frame.argument, lastRequestArgument) == 0) {
        SerialLock lock(pdMS_TO_TICKS(30));
        if (lock.held()) serialPrintln(lastResponse);
        return;
    }
    lastRequestOpcode = frame.opcode;
    strncpy(lastRequestArgument, frame.argument, sizeof(lastRequestArgument) - 1);
    lastRequestArgument[sizeof(lastRequestArgument) - 1] = '\0';

    switch (frame.opcode) {
        case SerialControlOpcode::HELLO: {
            char argument[64] = {};
#if defined(LORATRACE_BENCH_FAULTS)
            snprintf(argument, sizeof(argument), "V=%s;R=%s;CAP=USB;BENCH=1", FIRMWARE_VERSION,
                     FIRMWARE_BUILD_REV);
#else
            snprintf(argument, sizeof(argument), "V=%s;R=%s;CAP=USB", FIRMWARE_VERSION,
                     FIRMWARE_BUILD_REV);
#endif
            sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            break;
        }
        case SerialControlOpcode::STATUS:
            sendStatus(frame.sequence);
            break;
        case SerialControlOpcode::TRACE_SET:
            if (strcmp(frame.argument, "ACTIVE") == 0) {
                const bool accepted = radioRequestTracePause(false);
                sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                          accepted ? "ACTIVE" : "UNAVAILABLE");
            } else if (strcmp(frame.argument, "STANDBY") == 0) {
                const bool accepted = radioRequestTracePause(true);
                sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                          accepted ? "STANDBY" : "UNAVAILABLE");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
            }
            break;
        case SerialControlOpcode::PROFILE_SET: {
            MissionProfile profile;
            if (strcmp(frame.argument, "MESHTASTIC") == 0) profile = MissionProfile::MESHTASTIC;
            else if (strcmp(frame.argument, "MESHCORE") == 0) profile = MissionProfile::MESHCORE;
            else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            const bool accepted = radioRequestProfileSwitch(profile);
            sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                      accepted ? "QUEUED" : "UNAVAILABLE");
            break;
        }
        case SerialControlOpcode::PROBE_START:
            if (!loggerSdReady()) sendFrame(frame.sequence, SerialControlOpcode::ERROR, "SD_REQUIRED");
            else if (radioDiscoverySweepIsActive()) sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BUSY");
            else {
                const bool accepted = radioRequestDiscoverySweep();
                sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                          accepted ? "QUEUED" : "UNAVAILABLE");
            }
            break;
        case SerialControlOpcode::PROBE_CANCEL:
            if (!radioDiscoverySweepIsActive()) sendFrame(frame.sequence, SerialControlOpcode::ACK, "IDLE");
            else {
                const bool accepted = radioRequestDiscoverySweep();
                sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                          accepted ? "CANCEL_QUEUED" : "UNAVAILABLE");
            }
            break;
        case SerialControlOpcode::SWEEP_START:
            if (!loggerSdReady()) sendFrame(frame.sequence, SerialControlOpcode::ERROR, "SD_REQUIRED");
            else if (radioEnergySweepIsActive()) sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BUSY");
            else {
                const bool accepted = radioRequestEnergySweep();
                sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                          accepted ? "QUEUED" : "UNAVAILABLE");
            }
            break;
        case SerialControlOpcode::SWEEP_CANCEL:
            if (!radioEnergySweepIsActive()) sendFrame(frame.sequence, SerialControlOpcode::ACK, "IDLE");
            else {
                const bool accepted = radioRequestEnergySweep();
                sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                          accepted ? "CANCEL_QUEUED" : "UNAVAILABLE");
            }
            break;
        case SerialControlOpcode::BENCH_FAULT:
            if (benchFaultConfigure(frame.argument)) {
                sendFrame(frame.sequence, SerialControlOpcode::ACK,
                          strcmp(frame.argument, "CLEAR") == 0 ? "CLEARED" : "ARMED");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            }
            break;
        case SerialControlOpcode::BENCH_CAD:
            if (benchCadSymbolsConfigure(frame.argument)) {
                char argument[16] = {};
                snprintf(argument, sizeof(argument), "SYMS=%u", (unsigned)benchCadSymbols());
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            }
            break;
        case SerialControlOpcode::BENCH_SWEEP_MARGIN:
            if (benchSweepMarginConfigure(frame.argument)) {
                char argument[16] = {};
                snprintf(argument, sizeof(argument), "MARGIN=%d", (int)benchSweepMarginDbmX10());
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            }
            break;
        case SerialControlOpcode::BENCH_SWEEP_SETTLE:
            if (benchSweepSettleConfigure(frame.argument)) {
                char argument[20] = {};
                snprintf(argument, sizeof(argument), "SETTLE=%u",
                         (unsigned)benchSweepSettleMs());
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            }
            break;
        case SerialControlOpcode::BENCH_SWEEP_RETUNE:
            if (benchSweepRetuneConfigure(frame.argument)) {
                char argument[20] = {};
                snprintf(argument, sizeof(argument), "RETUNE=%s",
                         benchSweepRetuneFullEveryBin() ? "FULL" : "LIGHT");
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            }
            break;
        case SerialControlOpcode::BENCH_PASS_B_CAD: {
            uint16_t index = 0;
            if (!serialControlParseSequence(frame.argument, index) ||
                index >= PASS_B_SF_BW_CANDIDATE_COUNT) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
            } else if (!radioRequestBenchPassBCadTrigger((uint8_t)index)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "QUEUED");
            }
            break;
        }
        case SerialControlOpcode::BENCH_SWEEP_FLOOR: {
            uint16_t bin = 0;
            int16_t floor = 0;
            if (!serialControlParseSequence(frame.argument, bin)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
            } else if (!benchSweepFloorQuery(bin, floor)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            } else {
                char argument[24] = {};
                snprintf(argument, sizeof(argument), "BIN=%u;FLOOR=%d", (unsigned)bin, (int)floor);
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            }
            break;
        }
        case SerialControlOpcode::BENCH_RSSI_WINDOW: {
            uint32_t freqKhz = 0;
            if (!serialControlParseUint32(frame.argument, freqKhz) ||
                freqKhz < 860000UL || freqKhz > 930000UL) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
            } else if (!radioRequestBenchRssiWindow(freqKhz)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "QUEUED");
            }
            break;
        }
        case SerialControlOpcode::BENCH_RSSI_RESULT: {
            int16_t maxVal = 0, avgVal = 0;
            uint16_t sampleCount = 0;
            if (!radioBenchRssiWindowResult(maxVal, avgVal, sampleCount)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            } else {
                char argument[48] = {};
                snprintf(argument, sizeof(argument), "MAX=%d;AVG=%d;N=%u",
                         (int)maxVal, (int)avgVal, (unsigned)sampleCount);
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            }
            break;
        }
        case SerialControlOpcode::BENCH_FOCUS: {
            if (!benchFocusSurveyTriggerAllowed()) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
                break;
            }
            if (!loggerSdReady()) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "SD_REQUIRED");
                break;
            }
            char argument[40] = {};
            if (strlen(frame.argument) >= sizeof(argument)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            strcpy(argument, frame.argument);
            char *a = argument;
            char *b = strchr(a, ':');
            if (b == nullptr) { sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT"); break; }
            *b++ = '\0';
            char *c = strchr(b, ':');
            if (c == nullptr) { sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT"); break; }
            *c++ = '\0';
            // Optional 4th field: a one-shot sample-loop stall in ms, the only
            // way to reach Focus's timeout on demand (bench_fault.h). Omitting
            // it keeps the original bin:dwell:samples grammar working.
            char *d = strchr(c, ':');
            if (d != nullptr) *d++ = '\0';
            uint32_t bin, dwell, samples, stall = 0;
            if (!serialControlParseUint32(a, bin) || !serialControlParseUint32(b, dwell) ||
                !serialControlParseUint32(c, samples) || bin > UINT16_MAX || dwell > UINT16_MAX ||
                samples > UINT16_MAX ||
                (d != nullptr && !serialControlParseUint32(d, stall))) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            if (d != nullptr && !benchFocusStallConfigure(stall)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            FocusRequest request;
            request.region = radioEnergySweepRegion();
            request.selection_source = FocusSelectionSource::SWEEP_BIN;
            request.selection_bin_index = (uint16_t)bin;
            request.requested_dwell_ms = (uint16_t)dwell;
            request.requested_samples = (uint16_t)samples;
            request.requested_passes = FOCUS_BENCH_REQUESTED_PASSES;
            const bool accepted = radioRequestFocusSurvey(request);
            // A refused request never reaches Core 1's one-shot take, so the
            // stall would otherwise fire on whichever request ran next.
            if (!accepted) benchFocusStallConfigure(0);
            sendFrame(frame.sequence, accepted ? SerialControlOpcode::ACK : SerialControlOpcode::ERROR,
                      accepted ? "QUEUED" : "UNAVAILABLE");
            break;
        }
        case SerialControlOpcode::BENCH_FOCUS_CANCEL:
            if (!benchFocusSurveyTriggerAllowed()) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            } else if (radioCancelFocusSurvey()) {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "CANCEL_QUEUED");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "IDLE");
            }
            break;
        case SerialControlOpcode::BENCH_FOCUS_RESULT: {
            if (!benchFocusSurveyTriggerAllowed()) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
                break;
            }
            FocusObservation observation;
            if (!radioFocusLastObservation(observation)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "NO_RESULT");
                break;
            }
            if (strcmp(frame.argument, "HEALTH") == 0) {
                char argument[32] = {};
                snprintf(argument, sizeof(argument), "RS=%u;HR=%u;E=%d",
                         (unsigned)observation.request_status,
                         observation.home_restore ? 1U : 0U,
                         (int)observation.radio_status);
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
                break;
            }
            // Fixed-point dBm fields preserve exact device values without
            // locale/float formatting or leaking CSV's GPS/run columns.
            char argument[144] = {};
            snprintf(argument, sizeof(argument),
                     "ID=%u;BIN=%u;F=%lu;N=%u;MED=%d;P90=%d;MAX=%d;OBS=%lu;RS=%u;HR=%u;E=%d",
                     (unsigned)observation.focus_id,
                     (unsigned)observation.selection_bin_index,
                     (unsigned long)(observation.freq_mhz * 1000.0f),
                     (unsigned)observation.sample_count,
                     (int)observation.rssi_median_dbm_x10,
                     (int)observation.rssi_p90_dbm_x10,
                     (int)observation.rssi_peak_dbm_x10,
                     (unsigned long)observation.observation_ms,
                     (unsigned)observation.request_status,
                     observation.home_restore ? 1U : 0U,
                     (int)observation.radio_status);
            sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            break;
        }
        case SerialControlOpcode::BENCH_ACTION: {
            // Cell and Scope are menu-only actions in production, so a fixture
            // has no other way to prove Focus refuses them and is refused by
            // them (docs/research/phase12-survey-truth-design.md §8). This
            // calls the same request functions the menu does -- both toggle to
            // cancel while their own action is active -- and adds no radio
            // behavior of its own.
            if (!benchArbitrationTriggerAllowed()) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
                break;
            }
            if (strcmp(frame.argument, "STATE") == 0) {
                char argument[32] = {};
                snprintf(argument, sizeof(argument), "CELL=%u;SCOPE=%u;FOCUS=%u",
                         radioCellSweepIsActive() ? 1U : 0U,
                         radioScopeAcquireIsActive() ? 1U : 0U,
                         radioFocusSurveyIsActive() ? 1U : 0U);
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
                break;
            }
            const bool cell = strncmp(frame.argument, "CELL:", 5) == 0;
            const bool scope = strncmp(frame.argument, "SCOPE:", 6) == 0;
            if (!cell && !scope) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            const char *verb = frame.argument + (cell ? 5 : 6);
            const bool start = strcmp(verb, "START") == 0;
            if (!start && strcmp(verb, "CANCEL") != 0) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            const bool active = cell ? radioCellSweepIsActive() : radioScopeAcquireIsActive();
            if (start && active) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "ACTIVE");
                break;
            }
            if (!start && !active) {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "IDLE");
                break;
            }
            // Scope parks at one frequency; the resolved home channel keeps the
            // fixture from inventing one and keeps the hop short.
            const uint32_t scopeFreqKhz = (uint32_t)(radioActiveChannel().freq_mhz * 1000.0f);
            const bool accepted = cell ? radioRequestCellSweep()
                                       : radioRequestScopeAcquire(scopeFreqKhz);
            if (!accepted) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNAVAILABLE");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ACK,
                          start ? "QUEUED" : "CANCEL_QUEUED");
            }
            break;
        }
        case SerialControlOpcode::BENCH_PASS_B_CAD_RESULT: {
            EnergyObservationResult result;
            if (!benchPassBCadLastResult(result)) {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            } else {
                char argument[24] = {};
                snprintf(argument, sizeof(argument), "RESULT=%s", energyObservationResultName(result));
                sendFrame(frame.sequence, SerialControlOpcode::ACK, argument);
            }
            break;
        }
        case SerialControlOpcode::KEY_DUMP:
            // Diagnostic only: turns ui_task's raw TCA8418 event echo on or
            // off. Reads nothing and owns nothing, so it is safe to leave
            // armed; it is off at boot because the echo is noisy under any
            // real keyboard use.
            if (strcmp(frame.argument, "ON") == 0 || strcmp(frame.argument, "OFF") == 0) {
                const bool on = strcmp(frame.argument, "ON") == 0;
                uiKeyDumpSetEnabled(on);
                sendFrame(frame.sequence, SerialControlOpcode::ACK, on ? "ENABLED" : "DISABLED");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
            }
            break;
        case SerialControlOpcode::SD_RETRY:
            // Mirrors ui_actions.cpp's own MenuAction::SD_RETRY handler
            // exactly, so a remote request and the on-device menu item
            // behave identically.
            if (loggerSdReady()) {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "READY");
            } else if (loggerRequestSdRetry()) {
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "QUEUED");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNAVAILABLE");
            }
            break;
        case SerialControlOpcode::WIFI_SET:
            // Mirrors ui_actions.cpp's own MenuAction::WIFI_TOGGLE handler:
            // wifiToggle() only flips a *requested* flag, the actual state
            // change happens later on wifiTask's own Core 0 loop, so this
            // is fire-and-forget the same way -- poll STATUS's WIFI field
            // to see when it actually took effect.
            if (strcmp(frame.argument, "ON") == 0) {
                if (!wifiIsEnabled()) wifiToggle();
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "QUEUED_ON");
            } else if (strcmp(frame.argument, "OFF") == 0) {
                if (wifiIsEnabled()) wifiToggle();
                sendFrame(frame.sequence, SerialControlOpcode::ACK, "QUEUED_OFF");
            } else {
                sendFrame(frame.sequence, SerialControlOpcode::ERROR, "BAD_ARGUMENT");
            }
            break;
        case SerialControlOpcode::LOW_PROFILE_OFF:
            sendFrame(frame.sequence, SerialControlOpcode::ACK, "DISABLED");
            serialControlSetEnabled(false);
            break;
        default:
            sendFrame(frame.sequence, SerialControlOpcode::ERROR, "UNSUPPORTED");
            break;
    }
}

} // namespace

void serialControlSetEnabled(bool value) {
    enabled = value;
    Preferences preferences;
    if (preferences.begin(SERIAL_CONTROL_NVS_NAMESPACE, false)) {
        preferences.putBool(SERIAL_CONTROL_NVS_KEY, value);
        preferences.end();
    }
    persistedStateLoaded = true;
    clearInput();
    haveLastResponse = false;
    lastRequestOpcode = SerialControlOpcode::INVALID;
    lastRequestArgument[0] = '\0';
}

bool serialControlIsEnabled() {
    loadPersistedEnable();
    return enabled;
}

void serialControlPoll() {
    loadPersistedEnable();
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) break;
        if (c == '\n') {
            input[inputLength] = '\0';
            if (enabled) {
                SerialControlFrame frame;
                if (serialControlParseFrame(input, frame)) handleFrame(frame);
            }
            clearInput();
            continue;
        }
        if (c != '\r' && inputLength + 1 < sizeof(input)) {
            input[inputLength++] = (char)c;
        } else if (inputLength + 1 >= sizeof(input)) {
            clearInput();
        }
    }
}
