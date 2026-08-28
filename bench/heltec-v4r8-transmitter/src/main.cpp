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

void showStatus(bool force = false) {
    const uint32_t now = millis();
    if (!force && now - lastDisplayAtMs < DISPLAY_REFRESH_MS) return;
    lastDisplayAtMs = now;

    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, "LoRaTrace TX bench");
    display.drawStr(0, 24, radioReady ? "Radio: READY" : "Radio: INIT FAILED");
    display.drawStr(0, 38, active->name);
    display.drawStr(0, 52, armed ? "State: ARMED" : "State: IDLE");
    char event[24] = {};
    snprintf(event, sizeof(event), "Last: %s", lastEvent);
    display.drawStr(0, 63, event);
    display.sendBuffer();
}

void setArmed(bool value) {
    armed = value;
    digitalWrite(PIN_LED, armed ? HIGH : LOW);
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
        char status[48] = {};
        snprintf(status, sizeof(status), "C=%s;A=%u", active->name, armed ? 1U : 0U);
        reply(sequence, "STATUS", status);
    } else if (strcmp(command, "QUIET") == 0) {
        lastEvent = "QUIET";
        setArmed(false);
        reply(sequence, "ACK", "QUIET");
    } else if (strcmp(command, "CONFIG") == 0) {
        const Candidate *candidate = candidateByName(argument);
        if (candidate == nullptr) reply(sequence, "ERROR", "BAD_CANDIDATE");
        else if (!configure(candidate)) reply(sequence, "ERROR", "RADIO_INIT");
        else reply(sequence, "ACK", candidate->name);
    } else if (strcmp(command, "ARM") == 0) {
        uint16_t delayMs = 0;
        if (!parseU16(argument, delayMs) || delayMs > ARM_DELAY_MAX_MS) {
            reply(sequence, "ERROR", "BAD_DELAY");
        } else {
            armedSequence = sequence;
            armedAtMs = millis() + delayMs;
            lastEvent = "ARMED";
            setArmed(true);
            reply(sequence, "ACK", "ARMED");
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
    // The generic ESP32-S3 variant defaults I2C to GPIO8/9, which are this
    // board's LoRa NSS/SCK.  Bind the OLED bus before U8g2 touches Wire.
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 500000U);
    display.begin();
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
    if (armed && (int32_t)(millis() - armedAtMs) >= 0) transmitArmedPacket();
    showStatus();
}
