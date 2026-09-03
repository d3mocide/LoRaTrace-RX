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
    char argument[200] = {};
    snprintf(argument, sizeof(argument),
             "P=%s;T=%u;B=%s;SD=%u;F=%lu;R=%lu;I=%u;N=%u;C=%u,%u,%u,%u;M=%04X;"
             "W=%s;WI=%u;WN=%u;WP=%u;PBA=%lu;PBD=%lu;BPC=%u;RW=%u;WIFI=%u;EA=%lu;"
             "RXP=%lu;RXC=%lu",
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
             (unsigned long)radioPacketCount(), (unsigned long)radioCrcErrorCount());
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
