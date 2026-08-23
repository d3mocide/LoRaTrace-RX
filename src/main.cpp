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
#include <RadioLib.h>
#include <Arduino_GFX_Library.h>

#include "board_pins.h"
#include "channel_plans.h"
#include "config.h"
#include "io_expander.h"
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
// Arduino_ST7789's setRotation() (GFX Library for Arduino v1.4.0,
// Arduino_TFT.cpp) does NOT use offset pair 1 for portrait and pair 2 for
// landscape as board_pins.h previously assumed — at rotation 1 (what we
// use) it takes _xStart from ROW_OFFSET1 and _yStart from COL_OFFSET2,
// mixing one value from each pair. Passing only the "landscape" values
// into pair 1 left col_offset2/row_offset2 at their default of 0, so
// _yStart came out 0 instead of 53 — fillScreen() then cleared a window
// that didn't match the panel's actual visible glass, leaving Launcher's
// last-drawn screen visible in the untouched region (confirmed from a
// user photo 2026-08-22). All four offsets are needed; see board_pins.h.
Arduino_GFX *tft = new Arduino_ST7789(
    tftBus, PIN_TFT_RST, /* rotation= */ 1, /* ips= */ true, TFT_PANEL_WIDTH, TFT_PANEL_HEIGHT,
    TFT_COL_OFFSET_PORTRAIT, TFT_ROW_OFFSET_PORTRAIT, TFT_COL_OFFSET_LANDSCAPE, TFT_ROW_OFFSET_LANDSCAPE
);
bool displayReady = false;
int16_t splashY = 0;
int16_t rxLineY = 0; // Y of the reserved "Last RX" line, set once in setup()
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

// Redraws the one reserved "Last RX" line in place, each time a packet is
// decoded — the only splash content loop() updates beyond the heartbeat
// dot. fillRect first because print() doesn't erase whatever a previous,
// possibly-longer line left behind. Still one fixed line, no scrolling
// log, no interactivity.
void updateRxSplash(size_t len, float rssi, float snr) {
    if (!displayReady) return;
    tft->fillRect(0, rxLineY, tft->width(), SPLASH_LINE_H, SPLASH_BG);
    tft->setTextColor(SPLASH_FG, SPLASH_BG);
    tft->setCursor(4, rxLineY);
    tft->print("RX len=" + String(len) + " rssi=" + String(rssi, 0) + " snr=" + String(snr, 0));
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

void setup() {
    // Cardputer-ADV note recorded in board_pins.h (bmorcelli/Launcher's own
    // board notes): GPIO5 (PIN_LORA_NSS) needs to be driven high during
    // early GPIO init on this revision to avoid SD-mount interference from
    // the I2C device cluster on G8/G9 (antenna-switch IO-expander +
    // keyboard controller share those pins). Left as a no-op there since
    // our first hardware boot mounted SD fine without it. 2026-08-22's
    // second Launcher SD-drop boot then hit exactly that symptom (SD
    // `sdCommand(): crc error` / `GO_IDLE_STATE failed`), so applying it
    // now, ahead of any I2C or SPI access — otherwise NSS stays a floating
    // input (RadioLib doesn't configure it until radio.begin(), which runs
    // after the SD read) for that whole window. Hypothesis, not confirmed;
    // needs a reflash to know if it actually fixes the mount failure.
    pinMode(PIN_LORA_NSS, OUTPUT);
    digitalWrite(PIN_LORA_NSS, HIGH);

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

    // P0 high: enables the RF antenna switch AND powers the GPS module
    // (io_expander.h). Shared with the GPS probe so the two can't drift.
    if (!ioExpanderInit()) {
        Serial.println(F("FATAL: IO expander init failed — no ACK on I2C. Radio would be silent (and GPS unpowered) even if this continued."));
        splashLine(F("FATAL: IO expander"), SPLASH_ERR);
        splashLine(F("(antenna + GPS power)"), SPLASH_ERR);
        while (true) delay(1000);
    }
    Serial.println(F("IO expander: P0 high (antenna switch + GPS power)."));
    splashLine(F("IO expander: OK"));

    radioSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    // Optional channel override, e.g. for regional presets like
    // MeshOregon that don't run vanilla Meshtastic US LongFast. Fails
    // safe to the hardcoded default above if SD/file/values aren't good.
    bool sdOverrideApplied = loadChannelConfigFromSD(activeChannel, PIN_SD_CS, radioSPI);
    splashLine(sdOverrideApplied ? F("Config: SD override") : F("Config: default"));

    // Sync word is passed explicitly rather than left to RadioLib's default
    // (0x12). That default is *pre-1.2 Meshtastic*, not current Meshtastic
    // (0x2B) — see channel_plans.h for the upstream-source citation. Because
    // the SX126x only raises an RX interrupt on a sync-word match, running
    // the default meant this firmware could not hear modern Meshtastic
    // traffic at all, while still hearing unrelated 0x12 traffic: exactly the
    // "picks up random messages but misses my own node" symptom from the
    // 2026-08-23 bench tests. Everything after this arg keeps RadioLib's
    // defaults, same as before.
    int state = radio.begin(
        activeChannel.freq_mhz,
        activeChannel.bw_khz,
        activeChannel.sf,
        activeChannel.cr_denom,
        activeChannel.sync_word
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
    Serial.print(activeChannel.cr_denom);
    Serial.print(F(", sync 0x"));
    Serial.println(activeChannel.sync_word, HEX);
    splashLine(String(activeChannel.freq_mhz, 3) + "MHz SF" + activeChannel.sf);
    splashLine("BW" + String(activeChannel.bw_khz, 1) + " CR4/" + activeChannel.cr_denom +
               " sync 0x" + String(activeChannel.sync_word, HEX));

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

    // Reserve one line for the last received packet, updated in place by
    // updateRxSplash() from loop() — the only splash content that changes
    // after setup(), besides the heartbeat dot.
    rxLineY = splashY;
    splashLine(F("RX: none yet"));

    // PROGRESS.md/ROADMAP.md/DESIGN.md all flag real ESP.getFreeHeap()
    // (vs. the 250-380KB paper estimate, no-PSRAM chip) as an open
    // question blocking every later "does this fit in RAM" call — this
    // is the cheapest possible way to start closing it out.
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.print(F("Free heap: "));
    Serial.print(freeHeap);
    Serial.println(F(" bytes"));
    splashLine("Heap: " + String(freeHeap) + "B");

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
        // Previously a silent drop — logged now because a promiscuous
        // sniffer needs every DIO1 event accounted for, not just the ones
        // that decode cleanly (see PROGRESS.md 2026-08-23 missed-packet
        // investigation).
        Serial.print(F("[RX] dropped, bad length "));
        Serial.println(len);
        radio.startReceive();
        return;
    }

    int state = radio.readData(buf, len);

    // Read stats and re-arm RX before any Serial/display I/O below. Once
    // DIO1 fires the chip is out of Rx Continuous until startReceive() runs
    // again, so every microsecond spent printing/drawing here is a window
    // where a back-to-back packet (mesh relay, another node) goes unheard
    // — exactly the "sent 3, only 1 logged" symptom from bench testing next
    // to a live MeshOregon node (PROGRESS.md 2026-08-23). RSSI/SNR still
    // have to be read first: GetPacketStatus holds stats for the *last*
    // packet, and a new one arriving right after re-arming would overwrite
    // them before this line gets to read it. This narrows the deaf window
    // to a couple of SPI transactions; it doesn't eliminate it — that needs
    // the Core-1 radio task from DESIGN.md §9 phase 2.
    float rssi = 0, snr = 0;
    if (state == RADIOLIB_ERR_NONE) {
        rssi = radio.getRSSI();
        snr = radio.getSNR();
    }
    radio.startReceive();

    if (state == RADIOLIB_ERR_NONE) {
        Serial.print(F("[RX] len="));
        Serial.print(len);
        Serial.print(F(" rssi="));
        Serial.print(rssi);
        Serial.print(F("dBm snr="));
        Serial.print(snr);
        Serial.print(F("dB data="));
        for (size_t i = 0; i < len; i++) {
            if (buf[i] < 0x10) Serial.print('0');
            Serial.print(buf[i], HEX);
        }
        Serial.println();
        updateRxSplash(len, rssi, snr);
    } else {
        Serial.print(F("[RX] read error, code "));
        Serial.println(state);
    }
}
