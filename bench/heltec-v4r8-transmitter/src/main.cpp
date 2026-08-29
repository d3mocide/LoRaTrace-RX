#include <Arduino.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <string.h>

namespace {

constexpr size_t FRAME_MAX = 128;
constexpr int PIN_LORA_NSS = 8;
constexpr int PIN_LORA_SCK = 9;
constexpr int PIN_LORA_MOSI = 10;
constexpr int PIN_LORA_MISO = 11;
constexpr int PIN_LORA_RST = 12;
constexpr int PIN_LORA_BUSY = 13;
constexpr int PIN_LORA_DIO1 = 14;
constexpr int PIN_FEM_POWER = 7;
constexpr int PIN_FEM_ENABLE = 2;
constexpr int PIN_PA_CTX = 5; // V4 R8-specific PA control.
constexpr int PIN_VEXT = 40;  // V4 R8 display power rail, active low.
constexpr int PIN_OLED_SDA = 17;
constexpr int PIN_OLED_SCL = 18;
constexpr int PIN_OLED_RST = 21;
constexpr int PIN_LED = 46;
constexpr int8_t TX_POWER_DBM = -9;
constexpr uint32_t ARM_DELAY_MAX_MS = 5000;
constexpr uint32_t DISPLAY_REFRESH_MS = 250;
constexpr uint32_t SPLASH_HOLD_MS = 1500;

// Standalone RSSI sweep: an independent second energy-scan instrument,
// deliberately parameter-matched to the Cardputer's own ENERGY_SWEEP
// (src/energy_plan.h, src/energy_observation.h, radio_task.cpp's
// performEnergySweep()) so a Heltec-side sweep and a Cardputer-side sweep
// taken in the same room are directly comparable, not two different
// experiments. Band/step/margin below are copied from those files, not
// re-derived.
constexpr float SWEEP_BAND_LO_MHZ = 868.0f;
constexpr float SWEEP_STEP_MHZ = 0.25f;
constexpr uint16_t SWEEP_BIN_COUNT = 221; // energy_plan.h's 250kHz preset
constexpr uint8_t SWEEP_SAMPLES_PER_BIN = 4;
constexpr uint32_t SWEEP_SAMPLE_INTERVAL_MS = 1;
constexpr int16_t SWEEP_FLOOR_EMA_DIVISOR = 8;
constexpr int16_t SWEEP_MARGIN_DBM_X10 = 350; // 35.0dB, energy_observation.h's calibrated default
// Sweep always retunes using the MESH_OREGON physical tuple's modem params,
// matching the Cardputer's own default home channel -- not whatever TX
// candidate happens to be armed on this bench transmitter.
constexpr uint8_t SWEEP_SF = 8;
constexpr float SWEEP_BW_KHZ = 125.0f;
constexpr uint8_t SWEEP_CR_DENOM = 5;
constexpr uint8_t SWEEP_SYNC_WORD = 0x2B;

// Single-button (PRG/GPIO0, standard Heltec/ESP32-S3 boot button) menu,
// Meshtastic-style: a short press/release cycles the screen, a long
// (2s+) hold fires that screen's action. 2000ms is the hold duration
// already confirmed working on real hardware for the earlier
// hold-to-sweep behavior this replaces.
constexpr int PIN_BUTTON_PRG = 0;
// 2.0s -> 2.5s -> 1.5s (2026-08-28, both changes after physically testing
// on real hardware): 2.5s turned out to be too long a hold to trigger an
// action comfortably; 1.5s is the corrected value.
constexpr uint32_t BUTTON_LONG_PRESS_MS = 1500;
// A second, longer tier: hold through 5.0s (from any screen, not just the
// one showing) to immediately do what serial QUIET already does -- cancel
// an armed pulse and stop the beacon if running. Added alongside BEACON
// mode specifically because that mode now transmits untethered from the
// bench fixture, so a fast, unambiguous physical "stop everything" gesture
// matters more than it did when every mode here was USB-tethered.
constexpr uint32_t BUTTON_PANIC_STOP_MS = 5000;
constexpr uint16_t FIRE_BUTTON_DELAY_MS = 250; // same delay phase8_bench.py's own ARM calls typically use

enum class MenuScreen : uint8_t { HOME = 0, CANDIDATE, FIRE, SWEEP, BEACON, COUNT };

// Bench-only field/range-test beacon: periodic capped-count TX pulses at
// the selected candidate, for verifying Cardputer detection/range with a
// second device and no cable between them. Explicitly authorized
// 2026-08-28 as a deliberate supersession of the original "not an
// over-the-air field test transmitter" framing in
// research/phase8-low-profile-harness-design.md -- see
// research/heltec-standalone-sweep.md for the record. Still capped at the
// same -9dBm TX_POWER_DBM as every other mode here; bounded pulse count
// (not just a timer) so it can't be forgotten running indefinitely if the
// operator walks out of button/serial reach.
constexpr uint32_t BEACON_INTERVAL_MS = 2000;
constexpr uint16_t BEACON_MAX_PULSES = 120; // ~4 minutes at the interval above

struct Candidate {
    const char *name;
    float freq_mhz;
    uint8_t sf;
    float bw_khz;
    uint8_t cr_denom;
    uint8_t sync_word;
};

constexpr Candidate CANDIDATES[] = {
    {"LONG_FAST", 906.875f, 11, 250.0f, 5, 0x2B},
    {"LONG_MODERATE", 912.8125f, 11, 125.0f, 8, 0x2B},
    {"LONG_SLOW", 905.3125f, 12, 125.0f, 8, 0x2B},
    {"MEDIUM_FAST", 913.125f, 9, 250.0f, 5, 0x2B},
    {"MEDIUM_SLOW", 914.875f, 10, 250.0f, 5, 0x2B},
    {"SHORT_FAST", 918.875f, 7, 250.0f, 5, 0x2B},
    {"SHORT_SLOW", 920.625f, 8, 250.0f, 5, 0x2B},
    {"LONG_TURBO", 908.750f, 11, 500.0f, 8, 0x2B},
    {"MESH_OREGON", 918.5f, 8, 125.0f, 5, 0x2B},
};
constexpr size_t CANDIDATE_COUNT = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
const Candidate *active = &CANDIDATES[0];
char input[FRAME_MAX] = {};
size_t inputLength = 0;
bool armed = false;
uint16_t armedSequence = 0;
uint32_t armedAtMs = 0;
bool radioReady = false;
const char *lastEvent = "BOOT";
uint32_t lastDisplayAtMs = 0;
char lastSweepCounts[16] = "NONE";
MenuScreen currentScreen = MenuScreen::HOME;
bool buttonHeld = false;
bool buttonActionFired = false;
bool buttonPanicFired = false;
uint32_t buttonPressedAtMs = 0;
bool beaconActive = false;
uint16_t beaconPulseCount = 0;
uint32_t beaconNextAtMs = 0;

const char *screenLabel(MenuScreen screen) {
    switch (screen) {
        case MenuScreen::HOME: return "HOME";
        case MenuScreen::CANDIDATE: return "CANDIDATE";
        case MenuScreen::FIRE: return "FIRE";
        case MenuScreen::SWEEP: return "SWEEP";
        case MenuScreen::BEACON: return "BEACON";
        default: return "?";
    }
}

void drawMenu() {
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, screenLabel(currentScreen));

    char line1[24] = {};
    char line2[24] = {};
    char line3[24] = {};
    char line4[24] = {};
    bool haveLine4 = false;

    switch (currentScreen) {
        case MenuScreen::HOME:
            snprintf(line1, sizeof(line1), "%s", radioReady ? "Radio: READY" : "Radio: FAIL");
            snprintf(line2, sizeof(line2), "%s", active->name);
            snprintf(line3, sizeof(line3), "%s",
                     armed ? "State: ARMED" : (beaconActive ? "State: BEACON" : "State: IDLE"));
            snprintf(line4, sizeof(line4), "Last: %s", lastEvent);
            haveLine4 = true;
            break;
        case MenuScreen::CANDIDATE:
            snprintf(line1, sizeof(line1), "Hold: next candidate");
            snprintf(line2, sizeof(line2), "%s", active->name);
            snprintf(line3, sizeof(line3), "%.3fMHz SF%u", active->freq_mhz, (unsigned)active->sf);
            break;
        case MenuScreen::FIRE:
            snprintf(line1, sizeof(line1), "Hold: fire pulse");
            snprintf(line2, sizeof(line2), "%s", active->name);
            snprintf(line3, sizeof(line3), "%s", armed ? "ARMED" : "IDLE");
            break;
        case MenuScreen::SWEEP:
            // Sweep always retunes to the fixed MESH_OREGON params
            // (SWEEP_SF/SWEEP_BW_KHZ/SWEEP_CR_DENOM/SWEEP_SYNC_WORD above),
            // never `active` -- showing `active->name` here would falsely
            // imply the selected TX candidate controls what Sweep does
            // (caught by an operator question, 2026-08-28).
            snprintf(line1, sizeof(line1), "Hold: run sweep");
            snprintf(line2, sizeof(line2), "Last: %s", lastSweepCounts);
            snprintf(line3, sizeof(line3), "Fixed: MESH_OREGON");
            break;
        case MenuScreen::BEACON:
            snprintf(line1, sizeof(line1), "Hold: %s", beaconActive ? "stop" : "start");
            snprintf(line2, sizeof(line2), "Pulses: %u/%u", (unsigned)beaconPulseCount,
                     (unsigned)BEACON_MAX_PULSES);
            snprintf(line3, sizeof(line3), "%s", active->name);
            break;
        default:
            break;
    }

    display.drawStr(0, 24, line1);
    display.drawStr(0, 38, line2);
    display.drawStr(0, 52, line3);
    if (haveLine4) display.drawStr(0, 63, line4);
    display.sendBuffer();
}

void showSplash() {
    display.clearBuffer();
    display.setFont(u8g2_font_9x15B_tf);
    display.drawStr(14, 24, "LoRaTrace");
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(24, 42, "TX bench");
    display.drawStr(6, 58, "Heltec V4R8 / SX1262");
    display.sendBuffer();
}

void showStatus(bool force = false) {
    const uint32_t now = millis();
    if (!force && now - lastDisplayAtMs < DISPLAY_REFRESH_MS) return;
    lastDisplayAtMs = now;
    drawMenu();
}

void updateIndicatorLed() {
    digitalWrite(PIN_LED, (armed || beaconActive) ? HIGH : LOW);
}

void setArmed(bool value) {
    armed = value;
    updateIndicatorLed();
    showStatus(true);
}

uint16_t crc16(const char *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint8_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool parseU16(const char *text, uint16_t &out) {
    if (text == nullptr || !text[0]) return false;
    uint32_t value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10U + (uint32_t)(*p - '0');
        if (value > 65535U) return false;
    }
    out = (uint16_t)value;
    return true;
}

bool parseCrc(const char *text, uint16_t &out) {
    if (text == nullptr || strlen(text) != 4) return false;
    uint16_t value = 0;
    for (const char *p = text; *p; ++p) {
        uint8_t nibble;
        if (*p >= '0' && *p <= '9') nibble = (uint8_t)(*p - '0');
        else if (*p >= 'A' && *p <= 'F') nibble = (uint8_t)(*p - 'A' + 10);
        else return false;
        value = (uint16_t)((value << 4) | nibble);
    }
    out = value;
    return true;
}

void reply(uint16_t sequence, const char *event, const char *argument) {
    char body[FRAME_MAX] = {};
    const int n = snprintf(body, sizeof(body), "@LTTX/1 %u %s %s", (unsigned)sequence,
                           event, argument);
    if (n < 0 || (size_t)n >= sizeof(body)) return;
    Serial.printf("%s %04X\n", body, (unsigned)crc16(body, (size_t)n));
}

bool configure(const Candidate *candidate) {
    const int state = radio.begin(candidate->freq_mhz, candidate->bw_khz, candidate->sf,
                                  candidate->cr_denom, candidate->sync_word);
    if (state != RADIOLIB_ERR_NONE) return false;
    if (radio.setOutputPower(TX_POWER_DBM) != RADIOLIB_ERR_NONE) return false;
    if (radio.setPreambleLength(16) != RADIOLIB_ERR_NONE) return false;
    active = candidate;
    radioReady = true;
    lastEvent = "CONFIGURED";
    showStatus(true);
    return true;
}

const Candidate *candidateByName(const char *name) {
    for (const Candidate &candidate : CANDIDATES) {
        if (strcmp(name, candidate.name) == 0) return &candidate;
    }
    return nullptr;
}

void advanceCandidate() {
    size_t index = 0;
    for (size_t i = 0; i < CANDIDATE_COUNT; i++) {
        if (&CANDIDATES[i] == active) { index = i; break; }
    }
    index = (index + 1) % CANDIDATE_COUNT;
    configure(&CANDIDATES[index]);
}

void armPulse(uint16_t sequence, uint16_t delayMs) {
    armedSequence = sequence;
    armedAtMs = millis() + delayMs;
    lastEvent = "ARMED";
    setArmed(true);
}

// float dBm -> tenths-of-dBm fixed point, same rounding as
// src/energy_observation.h's energyRssiDbmToFixed() so the two devices'
// numbers are bit-for-bit comparable, not just close.
int16_t sweepRssiToFixed(float rssi_dbm) {
    float scaled = rssi_dbm * 10.0f;
    scaled = (scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f);
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32768.0f) scaled = -32768.0f;
    return (int16_t)scaled;
}

// Blocking by design: this is a manually-triggered bench diagnostic, not a
// background radio-task state, so there is no non-blocking API to preserve
// here the way there is on the Cardputer side.
void performSweep(uint16_t sequence, bool viaButton) {
    if (armed || beaconActive) {
        if (!viaButton) reply(sequence, "ERROR", armed ? "ARMED" : "BEACON_ACTIVE");
        return;
    }

    if (!viaButton) reply(sequence, "ACK", "SWEEP_STARTED");
    lastEvent = "SWEEPING";
    showStatus(true);
    Serial.println("#SWEEP START 868000 923000 221");

    int16_t noiseFloor = 0;
    bool haveFloor = false;
    uint16_t peakCount = 0;
    bool strongestValid = false;
    float strongestFreq = 0.0f;
    int16_t strongestRssiX10 = 0;

    for (uint16_t bin = 0; bin < SWEEP_BIN_COUNT; bin++) {
        const float freq = SWEEP_BAND_LO_MHZ + (float)bin * SWEEP_STEP_MHZ;
        const int beginState = radio.begin(freq, SWEEP_BW_KHZ, SWEEP_SF, SWEEP_CR_DENOM, SWEEP_SYNC_WORD);
        bool gotBin = false;
        int32_t sumX10 = 0;
        int16_t peakX10 = -32768;
        uint8_t samples = 0;
        if (beginState == RADIOLIB_ERR_NONE && radio.startReceive() == RADIOLIB_ERR_NONE) {
            gotBin = true;
            for (uint8_t s = 0; s < SWEEP_SAMPLES_PER_BIN; s++) {
                const int16_t fixed = sweepRssiToFixed(radio.getRSSI(false));
                sumX10 += fixed;
                if (fixed > peakX10) peakX10 = fixed;
                samples++;
                if (s + 1 < SWEEP_SAMPLES_PER_BIN) delay(SWEEP_SAMPLE_INTERVAL_MS);
            }
        }
        const int16_t avgX10 = gotBin ? (int16_t)(sumX10 / samples) : 0;
        bool isPeak = false;
        if (gotBin) {
            if (!haveFloor) {
                noiseFloor = avgX10;
                haveFloor = true;
            } else {
                isPeak = peakX10 >= (int16_t)(noiseFloor + SWEEP_MARGIN_DBM_X10);
                if (isPeak) {
                    peakCount++;
                    if (!strongestValid || peakX10 > strongestRssiX10) {
                        strongestValid = true;
                        strongestFreq = freq;
                        strongestRssiX10 = peakX10;
                    }
                }
                noiseFloor = (int16_t)(noiseFloor + (int32_t)(avgX10 - noiseFloor) / SWEEP_FLOOR_EMA_DIVISOR);
            }
        }
        Serial.printf("#SWEEP %u %lu %d %d %u %u\n", (unsigned)bin,
                      (unsigned long)(freq * 1000.0f + 0.5f), (int)avgX10, (int)peakX10,
                      (unsigned)(isPeak ? 1 : 0), (unsigned)(gotBin ? 1 : 0));

        if ((bin % 8) == 0 || bin + 1 == SWEEP_BIN_COUNT) {
            display.clearBuffer();
            display.setFont(u8g2_font_6x10_tf);
            display.drawStr(0, 10, "SWEEP");
            char line1[24] = {};
            snprintf(line1, sizeof(line1), "Bin %u/%u", (unsigned)(bin + 1), (unsigned)SWEEP_BIN_COUNT);
            display.drawStr(0, 24, line1);
            char line2[24] = {};
            snprintf(line2, sizeof(line2), "%.3f MHz", freq);
            display.drawStr(0, 38, line2);
            char line3[24] = {};
            snprintf(line3, sizeof(line3), "Peaks: %u", (unsigned)peakCount);
            display.drawStr(0, 52, line3);
            display.sendBuffer();
        }
    }

    configure(active); // restore the currently-armed TX candidate's modem config
    lastEvent = "SWEEP_DONE";
    showStatus(true);

    snprintf(lastSweepCounts, sizeof(lastSweepCounts), "%u/%u", (unsigned)peakCount,
             (unsigned)SWEEP_BIN_COUNT);
    char summary[64] = {};
    if (strongestValid) {
        snprintf(summary, sizeof(summary), "P=%s;F=%lu;R=%d", lastSweepCounts,
                 (unsigned long)(strongestFreq * 1000.0f + 0.5f), (int)strongestRssiX10);
    } else {
        snprintf(summary, sizeof(summary), "P=%s", lastSweepCounts);
    }
    reply(sequence, "SWEEP_DONE", summary);
    Serial.printf("#SWEEP DONE %s\n", summary);
}

// Shared by the serial QUIET command and the button's 5s panic-stop tier,
// so there is exactly one "stop everything" implementation rather than two
// copies that could quietly drift apart.
void quietAll() {
    lastEvent = "QUIET";
    setArmed(false);
    if (beaconActive) {
        beaconActive = false;
        updateIndicatorLed();
    }
}

void beaconToggle(uint16_t sequence, bool viaButton) {
    if (beaconActive) {
        beaconActive = false;
        updateIndicatorLed();
        lastEvent = "BEACON_STOPPED";
        showStatus(true);
        if (!viaButton) reply(sequence, "ACK", "BEACON_STOPPED");
        Serial.println("#BEACON STOPPED");
        return;
    }
    if (armed) {
        if (!viaButton) reply(sequence, "ERROR", "ARMED");
        return;
    }
    beaconActive = true;
    beaconPulseCount = 0;
    beaconNextAtMs = millis();
    updateIndicatorLed();
    lastEvent = "BEACON";
    showStatus(true);
    if (!viaButton) reply(sequence, "ACK", "BEACON_STARTED");
    Serial.println("#BEACON START");
}

void handle(char *line) {
    char *crcSeparator = strrchr(line, ' ');
    if (crcSeparator == nullptr || crcSeparator == line) return;
    uint16_t suppliedCrc = 0;
    if (!parseCrc(crcSeparator + 1, suppliedCrc)) return;
    *crcSeparator = '\0';
    if (crc16(line, strlen(line)) != suppliedCrc) return;

    char *save = nullptr;
    char *marker = strtok_r(line, " ", &save);
    char *sequenceText = strtok_r(nullptr, " ", &save);
    char *command = strtok_r(nullptr, " ", &save);
    char *argument = strtok_r(nullptr, " ", &save);
    if (marker == nullptr || sequenceText == nullptr || command == nullptr || argument == nullptr ||
        strtok_r(nullptr, " ", &save) != nullptr || strcmp(marker, "@LTTX/1") != 0) return;
    uint16_t sequence = 0;
    if (!parseU16(sequenceText, sequence)) return;

    if (strcmp(command, "HELLO") == 0) {
        reply(sequence, "ACK", "V4R8;SX1262;MAXM9");
    } else if (strcmp(command, "STATUS") == 0) {
        char status[64] = {};
        snprintf(status, sizeof(status), "C=%s;A=%u;SW=%s;B=%u", active->name, armed ? 1U : 0U,
                 lastSweepCounts, beaconActive ? 1U : 0U);
        reply(sequence, "STATUS", status);
    } else if (strcmp(command, "QUIET") == 0) {
        quietAll();
        reply(sequence, "ACK", "QUIET");
    } else if (strcmp(command, "CONFIG") == 0) {
        const Candidate *candidate = candidateByName(argument);
        if (candidate == nullptr) reply(sequence, "ERROR", "BAD_CANDIDATE");
        else if (!configure(candidate)) reply(sequence, "ERROR", "RADIO_INIT");
        else reply(sequence, "ACK", candidate->name);
    } else if (strcmp(command, "ARM") == 0) {
        uint16_t delayMs = 0;
        if (beaconActive) {
            reply(sequence, "ERROR", "BEACON_ACTIVE");
        } else if (!parseU16(argument, delayMs) || delayMs > ARM_DELAY_MAX_MS) {
            reply(sequence, "ERROR", "BAD_DELAY");
        } else {
            armPulse(sequence, delayMs);
            reply(sequence, "ACK", "ARMED");
        }
    } else if (strcmp(command, "SWEEP") == 0) {
        performSweep(sequence, false);
    } else if (strcmp(command, "BEACON") == 0) {
        if (strcmp(argument, "START") == 0) {
            if (beaconActive) reply(sequence, "ERROR", "ALREADY_ACTIVE");
            else beaconToggle(sequence, false);
        } else if (strcmp(argument, "STOP") == 0) {
            if (!beaconActive) reply(sequence, "ERROR", "NOT_ACTIVE");
            else beaconToggle(sequence, false);
        } else {
            reply(sequence, "ERROR", "BAD_ARG");
        }
    } else {
        reply(sequence, "ERROR", "UNSUPPORTED");
    }
}

void pollSerial() {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) return;
        if (c == '\n') {
            input[inputLength] = '\0';
            handle(input);
            inputLength = 0;
        } else if (c != '\r' && inputLength + 1 < sizeof(input)) {
            input[inputLength++] = (char)c;
        } else if (inputLength + 1 >= sizeof(input)) {
            inputLength = 0;
        }
    }
}

// The screen currently shown decides what a long press does -- Meshtastic's
// own single-button convention. HOME is a pure overview (no action). Every
// action here checks its own busy-guard rather than trusting the caller, so
// stray/rapid button input can't bypass the same rules a host command
// would follow.
void runScreenAction() {
    switch (currentScreen) {
        case MenuScreen::HOME:
            break;
        case MenuScreen::CANDIDATE:
            if (!armed && !beaconActive) advanceCandidate();
            break;
        case MenuScreen::FIRE:
            if (!armed && !beaconActive) armPulse(0, FIRE_BUTTON_DELAY_MS);
            break;
        case MenuScreen::SWEEP:
            performSweep(0, true);
            break;
        case MenuScreen::BEACON:
            beaconToggle(0, true);
            break;
        default:
            break;
    }
}

void pollButton() {
    const bool pressed = digitalRead(PIN_BUTTON_PRG) == LOW;
    if (pressed && !buttonHeld) {
        buttonHeld = true;
        buttonPressedAtMs = millis();
        buttonActionFired = false;
        buttonPanicFired = false;
    } else if (pressed && buttonHeld) {
        const uint32_t held = millis() - buttonPressedAtMs;
        // Two independent timed thresholds during one hold, not mutually
        // exclusive: the 2.5s screen action still fires on the way to a
        // continued 5s hold, then the panic-stop also fires -- e.g. holding
        // through 5s on the BEACON screen starts it at 2.5s and immediately
        // stops it again at 5s. No action needs to be "undone" this way,
        // and the two tiers stay simple, independent checks.
        if (!buttonActionFired && held >= BUTTON_LONG_PRESS_MS) {
            buttonActionFired = true;
            runScreenAction();
        }
        if (!buttonPanicFired && held >= BUTTON_PANIC_STOP_MS) {
            buttonPanicFired = true;
            quietAll();
            showStatus(true);
            Serial.println("#BUTTON PANIC_STOP");
        }
    } else if (!pressed && buttonHeld) {
        if (!buttonActionFired) {
            currentScreen = (MenuScreen)(((uint8_t)currentScreen + 1) % (uint8_t)MenuScreen::COUNT);
            showStatus(true);
        }
        buttonHeld = false;
        buttonActionFired = false;
        buttonPanicFired = false;
    }
}

void transmitArmedPacket() {
    setArmed(false);
    char payload[48] = {};
    snprintf(payload, sizeof(payload), "LTTX,%u,%lu", (unsigned)armedSequence,
             (unsigned long)millis());
    reply(armedSequence, "TX_STARTED", active->name);
    const int state = radio.transmit(payload);
    lastEvent = state == RADIOLIB_ERR_NONE ? "TX OK" : "TX ERROR";
    showStatus(true);
    reply(armedSequence, state == RADIOLIB_ERR_NONE ? "TX_DONE" : "ERROR",
          state == RADIOLIB_ERR_NONE ? "OK" : "RADIO_TX");
}

void serviceBeacon() {
    if (!beaconActive) return;
    if ((int32_t)(millis() - beaconNextAtMs) < 0) return;

    beaconPulseCount++;
    char payload[48] = {};
    snprintf(payload, sizeof(payload), "LTBEACON,%u,%lu", (unsigned)beaconPulseCount,
             (unsigned long)millis());
    const int state = radio.transmit(payload);
    Serial.printf("#BEACON %u %s %lu %s\n", (unsigned)beaconPulseCount, active->name,
                  (unsigned long)(active->freq_mhz * 1000.0f + 0.5f),
                  state == RADIOLIB_ERR_NONE ? "OK" : "ERROR");

    if (beaconPulseCount >= BEACON_MAX_PULSES) {
        beaconActive = false;
        updateIndicatorLed();
        lastEvent = "BEACON_DONE";
        showStatus(true);
        Serial.println("#BEACON DONE MAX_PULSES");
    } else {
        beaconNextAtMs = millis() + BEACON_INTERVAL_MS;
        showStatus(true);
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t started = millis();
    while (!Serial && millis() - started < 3000) {}

    SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    pinMode(PIN_OLED_RST, OUTPUT);
    digitalWrite(PIN_OLED_RST, LOW);
    delay(20);
    digitalWrite(PIN_OLED_RST, HIGH);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BUTTON_PRG, INPUT_PULLUP);
    // The generic ESP32-S3 variant defaults I2C to GPIO8/9, which are this
    // board's LoRa NSS/SCK.  Bind the OLED bus before U8g2 touches Wire.
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 500000U);
    display.begin();
    showSplash();
    delay(SPLASH_HOLD_MS);
    showStatus(true);
    pinMode(PIN_FEM_POWER, OUTPUT);
    pinMode(PIN_FEM_ENABLE, OUTPUT);
    pinMode(PIN_PA_CTX, OUTPUT);
    digitalWrite(PIN_FEM_POWER, HIGH);
    digitalWrite(PIN_FEM_ENABLE, HIGH);
    digitalWrite(PIN_PA_CTX, HIGH);
    delay(2);

    Serial.println("[lttx] Heltec V4 R8 deterministic transmitter");
    if (configure(active)) {
        Serial.println("[lttx] radio ready; output capped at -9 dBm");
    } else {
        radioReady = false;
        lastEvent = "RADIO INIT FAIL";
        showStatus(true);
        Serial.println("[lttx] radio init failed");
    }
}

void loop() {
    pollSerial();
    pollButton();
    serviceBeacon();
    if (armed && (int32_t)(millis() - armedAtMs) >= 0) transmitArmedPacket();
    showStatus();
}
