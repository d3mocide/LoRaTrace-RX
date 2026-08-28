#include "low_profile.h"

#include <Arduino.h>
#include <string.h>

#include "logger_task.h"
#include "low_profile_protocol.h"
#include "radio_task.h"
#include "serial_lock.h"
#include "version.h"

namespace {

volatile bool enabled = false;
char input[LOW_PROFILE_FRAME_MAX] = {};
size_t inputLength = 0;
uint16_t lastSequence = 0;
char lastResponse[LOW_PROFILE_FRAME_MAX] = {};
bool haveLastResponse = false;
LowProfileOpcode lastRequestOpcode = LowProfileOpcode::INVALID;
char lastRequestArgument[80] = {};

void clearInput() {
    inputLength = 0;
    input[0] = '\0';
}

void sendFrame(uint16_t sequence, LowProfileOpcode opcode, const char *argument,
               bool cache = true) {
    char frame[LOW_PROFILE_FRAME_MAX] = {};
    if (lowProfileFormatFrame(frame, sizeof(frame), sequence, opcode, argument) == 0) return;
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
    const ChannelParams channel = radioActiveChannel();
    char argument[80] = {};
    snprintf(argument, sizeof(argument), "P=%s;T=%u;B=%s;SD=%u;F=%lu;R=%lu;I=%u;N=%u", profile,
             radioIsTracePaused() ? 0U : 1U, probe, loggerSdReady() ? 1U : 0U,
             (unsigned long)(channel.freq_mhz * 1000.0f),
             (unsigned long)radioDiscoveryRecoveryCount(),
             (unsigned)radioDiscoveryCandidateIndex(),
             (unsigned)radioDiscoveryCandidateCount());
    sendFrame(sequence, LowProfileOpcode::STATUS, argument);
}

void handleFrame(const LowProfileFrame &frame) {
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
        case LowProfileOpcode::HELLO: {
            char argument[48] = {};
            snprintf(argument, sizeof(argument), "V=%s;R=%s;CAP=USB", FIRMWARE_VERSION,
                     FIRMWARE_BUILD_REV);
            sendFrame(frame.sequence, LowProfileOpcode::ACK, argument);
            break;
        }
        case LowProfileOpcode::STATUS:
            sendStatus(frame.sequence);
            break;
        case LowProfileOpcode::TRACE_SET:
            if (strcmp(frame.argument, "ACTIVE") == 0) {
                const bool accepted = radioRequestTracePause(false);
                sendFrame(frame.sequence, accepted ? LowProfileOpcode::ACK : LowProfileOpcode::ERROR,
                          accepted ? "ACTIVE" : "UNAVAILABLE");
            } else if (strcmp(frame.argument, "STANDBY") == 0) {
                const bool accepted = radioRequestTracePause(true);
                sendFrame(frame.sequence, accepted ? LowProfileOpcode::ACK : LowProfileOpcode::ERROR,
                          accepted ? "STANDBY" : "UNAVAILABLE");
            } else {
                sendFrame(frame.sequence, LowProfileOpcode::ERROR, "BAD_ARGUMENT");
            }
            break;
        case LowProfileOpcode::PROFILE_SET: {
            MissionProfile profile;
            if (strcmp(frame.argument, "MESHTASTIC") == 0) profile = MissionProfile::MESHTASTIC;
            else if (strcmp(frame.argument, "MESHCORE") == 0) profile = MissionProfile::MESHCORE;
            else {
                sendFrame(frame.sequence, LowProfileOpcode::ERROR, "BAD_ARGUMENT");
                break;
            }
            const bool accepted = radioRequestProfileSwitch(profile);
            sendFrame(frame.sequence, accepted ? LowProfileOpcode::ACK : LowProfileOpcode::ERROR,
                      accepted ? "QUEUED" : "UNAVAILABLE");
            break;
        }
        case LowProfileOpcode::PROBE_START:
            if (!loggerSdReady()) sendFrame(frame.sequence, LowProfileOpcode::ERROR, "SD_REQUIRED");
            else if (radioDiscoverySweepIsActive()) sendFrame(frame.sequence, LowProfileOpcode::ERROR, "BUSY");
            else {
                const bool accepted = radioRequestDiscoverySweep();
                sendFrame(frame.sequence, accepted ? LowProfileOpcode::ACK : LowProfileOpcode::ERROR,
                          accepted ? "QUEUED" : "UNAVAILABLE");
            }
            break;
        case LowProfileOpcode::PROBE_CANCEL:
            if (!radioDiscoverySweepIsActive()) sendFrame(frame.sequence, LowProfileOpcode::ACK, "IDLE");
            else {
                const bool accepted = radioRequestDiscoverySweep();
                sendFrame(frame.sequence, accepted ? LowProfileOpcode::ACK : LowProfileOpcode::ERROR,
                          accepted ? "CANCEL_QUEUED" : "UNAVAILABLE");
            }
            break;
        case LowProfileOpcode::LOW_PROFILE_OFF:
            sendFrame(frame.sequence, LowProfileOpcode::ACK, "DISABLED");
            lowProfileSetEnabled(false);
            break;
        default:
            sendFrame(frame.sequence, LowProfileOpcode::ERROR, "UNSUPPORTED");
            break;
    }
}

} // namespace

void lowProfileSetEnabled(bool value) {
    enabled = value;
    clearInput();
    haveLastResponse = false;
    lastRequestOpcode = LowProfileOpcode::INVALID;
    lastRequestArgument[0] = '\0';
}

bool lowProfileIsEnabled() {
    return enabled;
}

void lowProfilePoll() {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) break;
        if (c == '\n') {
            input[inputLength] = '\0';
            if (enabled) {
                LowProfileFrame frame;
                if (lowProfileParseFrame(input, frame)) handleFrame(frame);
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
