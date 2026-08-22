// LoRaTrace RX — Phase 1 bring-up (DESIGN.md §9, step 1)
//
// Scope, deliberately: RadioLib talking to the SX1262 on its own SPI host,
// IO-expander antenna switch confirmed, RX on the Meshtastic LongFast (US)
// channel by default — optionally overridden from an SD config file for
// non-default regional presets (config.h) — detections printed to Serial.
// No task/queue architecture, no GPS, no full SD logging, no UI yet —
// CLAUDE.md is explicit that those wait until this bring-up step is
// proven on real hardware. The SD config read is a narrow, deliberate
// exception: a one-shot boot-time read, not the Logger task.
//
// UNTESTED ON HARDWARE: written and reasoned through against RadioLib's
// documented API and the M5Stack/PI4IOE5V6408 facts recorded in
// board_pins.h, but this repo has no board attached to flash against. See
// PROGRESS.md "Phase 1" checklist for what still needs bench verification
// before trusting this blind — SPI host availability, IO-expander register
// behavior, and RadioLib's exact call signatures for whatever version
// actually resolves at build time chief among them.

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <Arduino_GFX_Library.h>

#include "board_pins.h"
#include "channel_plans.h"
#include "config.h"
#include "version.h"

// Dedicated SPI host for the SX1262, isolated from the display bus
// (DESIGN.md §1: shared-bus display refreshes jitter CAD timing). Also
// shared with the microSD card (PIN_SD_CS) — see board_pins.h; SD and
// radio never touch the bus concurrently in this phase, only sequentially
// during setup(), so that sharing is safe here even though it'll need
// real arbitration once Phase 2 makes both active at once.
SPIClass radioSPI(FSPI);
SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, radioSPI);

// Boot-status splash — PROGRESS.md decisions log: a narrow, deliberate
// Phase-1 exception to CLAUDE.md's "no UI yet," not the Phase 6 ui_task.
// Written once per setup() milestone, never touched from loop() — no
// keyboard reading, no menus, no redraw loop. Own SPI host (HSPI), fully
// disjoint pins from radioSPI/SD (board_pins.h), so no bus-sharing concerns
// like the radio/SD pair has. If initDisplay() fails, splashLine() below
// silently no-ops and boot proceeds exactly as it did before this existed.
SPIClass tftSPI(HSPI);
Arduino_DataBus *tftBus = new Arduino_HWSPI(
    PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCLK, PIN_TFT_MOSI, -1 /* MISO unused, display is write-only */, &tftSPI
);
Arduino_GFX *tft = new Arduino_ST7789(
    tftBus, PIN_TFT_RST, /* rotation= */ 1, /* ips= */ true, TFT_PANEL_WIDTH, TFT_PANEL_HEIGHT,
    TFT_COL_OFFSET_LANDSCAPE, TFT_ROW_OFFSET_LANDSCAPE
);
bool displayReady = false;
int16_t splashY = 0;
constexpr uint16_t SPLASH_BG = 0x0000;  // RGB565 black
constexpr uint16_t SPLASH_FG = 0xFFFF;  // RGB565 white
constexpr uint16_t SPLASH_ERR = 0xF800; // RGB565 red
constexpr uint8_t SPLASH_LINE_H = 10;   // px, text size 1

// Meshtastic LongFast (US) unless overridden by /loratrace/config.txt on
// SD — see config.h/config.cpp and sd-template/loratrace/config.txt.
ChannelParams activeChannel = CHANNEL_MESHTASTIC_LONGFAST_US;

volatile bool packetReady = false;

void IRAM_ATTR onPacketReceived() {
    packetReady = true;
}

// Appends one line to the splash. Safe to call unconditionally — a no-op
// whenever the display didn't come up (displayReady false).
void splashLine(const String &msg, uint16_t color = SPLASH_FG) {
    if (!displayReady) return;
    tft->setTextColor(color, SPLASH_BG);
    tft->setCursor(4, splashY);
    tft->println(msg);
    splashY += SPLASH_LINE_H;
}

// Brings up the ST7789 so setup()'s progress is visible without a serial
// connection open (PROGRESS.md: booted hardware "looks like a brick"
// without one). Pins/offsets are sourced, not bench-verified — see
// board_pins.h. Best-effort: returns false rather than blocking boot if
// the panel doesn't respond.
bool initDisplay() {
    tftSPI.begin(PIN_TFT_SCLK, -1 /* MISO unused */, PIN_TFT_MOSI, PIN_TFT_CS);
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    if (!tft->begin()) return false;
    tft->fillScreen(SPLASH_BG);
    tft->setTextSize(1);
    return true;
}

// Toggles a small dot in the bottom-right corner every ~500ms. The splash
// above is otherwise fully static once setup() finishes, which makes a
// genuinely hung device (stuck in a FATAL while(true) loop, or wedged
// somewhere before ever reaching loop()) look identical to a healthy idle
// one on screen. This only ever ticks from inside loop() — it freezes
// right alongside everything else if loop() stops running, which is the
// point: it's a liveliness signal for the whole firmware, not decoration.
// Still passive/non-interactive — no keyboard reading, not the ui_task.
void heartbeatTick() {
    if (!displayReady) return;
    static unsigned long lastToggle = 0;
    static bool dotOn = false;
    unsigned long now = millis();
    if (now - lastToggle < 500) return;
    lastToggle = now;
    dotOn = !dotOn;
    tft->fillCircle(tft->width() - 8, tft->height() - 8, 3, dotOn ? SPLASH_FG : SPLASH_BG);
}

// Drive P0 on the PI4IOE5V6408 high once at boot. Without this the radio
// is silent regardless of everything else being correct (DESIGN.md §1).
// Every register is written explicitly rather than relying on power-on
// defaults — see board_pins.h for the verify status of this register map.
bool initAntennaSwitch() {
    Wire.begin(PIN_IOEXP_SDA, PIN_IOEXP_SCL);

    const uint8_t antMask = 1 << IOEXP_ANT_SWITCH_BIT;

    // P0 = output, leave other pins as inputs.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_IO_DIRECTION);
    Wire.write(antMask);
    if (Wire.endTransmission() != 0) return false;

    // Disable high-Z on P0 so the output actually drives the pin.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_HIGH_Z);
    Wire.write(static_cast<uint8_t>(~antMask)); // 0 = not high-Z for P0
    if (Wire.endTransmission() != 0) return false;

    // Drive P0 high.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_OUTPUT_STATE);
    Wire.write(antMask);
    if (Wire.endTransmission() != 0) return false;

    return true;
}

void setup() {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000) {}

    Serial.print(F("LoRaTrace RX v"));
    Serial.print(FIRMWARE_VERSION);
    Serial.println(F(" — phase 1 bring-up"));

    displayReady = initDisplay();
    tft->setTextSize(2);
    splashLine(F("LoRaTrace RX"));
    splashY += SPLASH_LINE_H; // size-2 glyphs are taller than SPLASH_LINE_H
    tft->setTextSize(1);
    splashLine(String("v") + FIRMWARE_VERSION + " -- phase 1");
    splashY += SPLASH_LINE_H / 2; // small gap under the title

    if (!initAntennaSwitch()) {
        Serial.println(F("FATAL: IO expander (antenna switch) init failed — no ACK on I2C. Radio would be silent even if this continued."));
        splashLine(F("FATAL: antenna switch"), SPLASH_ERR);
        splashLine(F("(IO-expander I2C failed)"), SPLASH_ERR);
        while (true) delay(1000);
    }
    Serial.println(F("Antenna switch: P0 driven high."));
    splashLine(F("Antenna switch: OK"));

    radioSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    // Optional channel override, e.g. for regional presets like
    // MeshOregon that don't run vanilla Meshtastic US LongFast. Fails
    // safe to the hardcoded default above if SD/file/values aren't good.
    loadChannelConfigFromSD(activeChannel, PIN_SD_CS, radioSPI);

    int state = radio.begin(
        activeChannel.freq_mhz,
        activeChannel.bw_khz,
        activeChannel.sf,
        activeChannel.cr_denom
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("FATAL: SX1262 init failed, code "));
        Serial.println(state);
        splashLine("FATAL: SX1262 init, code " + String(state), SPLASH_ERR);
        while (true) delay(1000);
    }
    Serial.println(F("SX1262 initialized."));
    splashLine(F("SX1262: OK"));
    Serial.print(F("Active channel: "));
    Serial.print(activeChannel.freq_mhz, 3);
    Serial.print(F(" MHz, SF"));
    Serial.print(activeChannel.sf);
    Serial.print(F(", BW"));
    Serial.print(activeChannel.bw_khz, 1);
    Serial.print(F("kHz, CR4/"));
    Serial.println(activeChannel.cr_denom);
    splashLine(String(activeChannel.freq_mhz, 3) + "MHz SF" + activeChannel.sf);
    splashLine("BW" + String(activeChannel.bw_khz, 1) + " CR4/" + activeChannel.cr_denom);

    radio.setDio1Action(onPacketReceived);

    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("FATAL: startReceive failed, code "));
        Serial.println(state);
        splashLine("FATAL: startReceive, code " + String(state), SPLASH_ERR);
        while (true) delay(1000);
    }

    Serial.println(F("Listening..."));
    splashY += SPLASH_LINE_H / 2;
    splashLine(F("Listening..."));

    // Launcher (bmorcelli/Launcher, SD-drop install path — ROADMAP.md
    // Distribution) auto-reboots into whatever ran last unless a key is
    // pressed during its own ~5s boot window (it prints its own "Press the
    // button to enter the Launcher!" over serial during that window, not
    // this firmware). Confirmed against Launcher's own source, not a
    // guess — see PROGRESS.md. Its Settings -> "Boot to Launcher" toggle
    // removes the timing entirely: once enabled, Launcher always stops at
    // its menu on reset instead of auto-booting the last app.
    Serial.println(F("To return to Launcher: press any key during its ~5s boot window, or enable its Settings -> \"Boot to Launcher\" toggle."));
}

void loop() {
    heartbeatTick();

    if (!packetReady) return;
    packetReady = false;

    uint8_t buf[256];
    size_t len = radio.getPacketLength();
    if (len == 0 || len > sizeof(buf)) {
        radio.startReceive();
        return;
    }

    int state = radio.readData(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
        Serial.print(F("[RX] len="));
        Serial.print(len);
        Serial.print(F(" rssi="));
        Serial.print(radio.getRSSI());
        Serial.print(F("dBm snr="));
        Serial.print(radio.getSNR());
        Serial.println(F("dB"));
    } else {
        Serial.print(F("[RX] read error, code "));
        Serial.println(state);
    }

    // No GPS/SD fusion yet (Phase 2) — this is a serial-only smoke test
    // of the radio path per DESIGN.md §9 step 1.
    radio.startReceive();
}
