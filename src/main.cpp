// LoRaTrace RX — Phase 2: task/queue architecture, GPS, SD logging.
//
// DESIGN.md §9 step 2 / ROADMAP.md Phase 2 (MVP-Beta). This file is now an
// orchestrator, not a driver: it brings up hardware in a fixed order, then
// hands the work to three tasks and gets out of the way.
//
//   Core 1: radio_task   — owns the SX1262, HOME_LISTEN, never blocks
//   Core 0: gps_task     — NMEA -> last-fix behind a mutex
//   Core 0: logger_task  — dequeue, GPS-stamp, batched SD writes
//
// Detections cross cores through a FreeRTOS queue as ~36-byte structs
// (detection.h). SD and the SX1262 share one physical SPI bus, so both
// tasks arbitrate through spi_bus.h — the queue alone is not enough, since
// two devices on one bus cannot transact at the same instant no matter
// which core issues them.
//
// loop() keeps only what genuinely belongs on the main task: the display
// heartbeat and a periodic status line. Real UI is still Phase 6.
//
// Boot order matters and is not arbitrary:
//   1. NSS high      — before any I2C/SPI, or SD mounts unreliably
//   2. IO expander   — antenna switch AND GPS power (io_expander.h)
//   3. SPI bus       — mutex + peripheral, before anything touches it
//   4. SD config     — one-shot channel override, before the radio starts
//   5. Tasks         — radio last, so the queue exists before packets do

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "battery.h"
#include "board_pins.h"
#include "channel_plans.h"
#include "config.h"
#include "detection.h"
#include "gps_task.h"
#include "io_expander.h"
#include "logger_task.h"
#include "radio_task.h"
#include "spi_bus.h"
#include "ui_task.h"
#include "version.h"

// Boot-status splash — PROGRESS.md decisions log: a narrow, deliberate
// exception to CLAUDE.md's "no UI yet," not the Phase 6 ui_task. Own SPI
// host (HSPI), fully disjoint pins from the radio/SD bus (board_pins.h), so
// it needs no spi_bus arbitration. If initDisplay() fails, splashLine()
// silently no-ops and boot proceeds exactly as it would have.
SPIClass tftSPI(HSPI);
Arduino_DataBus *tftBus = new Arduino_HWSPI(
    PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCLK, PIN_TFT_MOSI, -1 /* MISO unused, display is write-only */, &tftSPI
);
// All four IPS offsets are required: Arduino_ST7789's setRotation() mixes
// one value from each pair at rotation 1, so omitting the second pair
// silently zeroes _yStart and leaves part of the panel uncleared. See
// board_pins.h for the per-rotation mapping.
Arduino_GFX *tft = new Arduino_ST7789(
    tftBus, PIN_TFT_RST, /* rotation= */ 1, /* ips= */ true, TFT_PANEL_WIDTH, TFT_PANEL_HEIGHT,
    TFT_COL_OFFSET_PORTRAIT, TFT_ROW_OFFSET_PORTRAIT, TFT_COL_OFFSET_LANDSCAPE, TFT_ROW_OFFSET_LANDSCAPE
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

// Detection queue, Core 1 -> Core 0. 32 entries x 36B = ~1.2KB: deep enough
// to ride out an SD flush (including the back-to-back original/rebroadcast
// pairs that Meshtastic flooding produces), small enough to respect the
// no-PSRAM budget in DESIGN.md §1. Depth is a real tuning knob — watch
// radioQueueDropCount() in the status line before changing it.
constexpr UBaseType_t DETECTION_QUEUE_DEPTH = 32;
QueueHandle_t detectionQueue = nullptr;

void splashLine(const String &msg, uint16_t color = SPLASH_FG) {
    if (!displayReady) return;
    tft->setTextColor(color, SPLASH_BG);
    tft->setCursor(4, splashY);
    tft->println(msg);
    splashY += SPLASH_LINE_H;
}

bool initDisplay() {
    tftSPI.begin(PIN_TFT_SCLK, -1 /* MISO unused */, PIN_TFT_MOSI, PIN_TFT_CS);
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    if (!tft->begin()) return false;
    tft->fillScreen(SPLASH_BG);
    tft->setTextSize(1);
    return true;
}

// The boot-time liveness heartbeat now lives in ui_task (it owns the panel
// after setup). Keeping a copy here would race it for the same pixels.

void fatal(const __FlashStringHelper *serialMsg, const __FlashStringHelper *splashMsg) {
    Serial.println(serialMsg);
    splashLine(splashMsg, SPLASH_ERR);
    while (true) delay(1000);
}

void setup() {
    // GPIO5 (PIN_LORA_NSS) must be driven high before any I2C or SPI access
    // on this board revision, or the SD card intermittently fails to mount
    // (`sdCommand(): crc error`). Confirmed to help on hardware; see
    // board_pins.h and PROGRESS.md.
    pinMode(PIN_LORA_NSS, OUTPUT);
    digitalWrite(PIN_LORA_NSS, HIGH);

    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000) {}

    Serial.print(F("LoRaTrace RX v"));
    Serial.print(FIRMWARE_VERSION);
    Serial.print(F(" ("));
    Serial.print(FIRMWARE_BUILD_REV); // git SHA — identifies THIS binary
    Serial.println(F(") — phase 2 (tasks + GPS + SD logging)"));

    displayReady = initDisplay();
    tft->setTextSize(2);
    splashLine(F("LoRaTrace RX"));
    splashY += SPLASH_LINE_H;
    tft->setTextSize(1);
    splashLine(String("v") + FIRMWARE_VERSION + " -- phase 2");
    splashY += SPLASH_LINE_H / 2;

    // P0 high: RF antenna switch AND GPS power. Fatal because both halves
    // of this device's job depend on it.
    if (!ioExpanderInit()) {
        fatal(F("FATAL: IO expander init failed — no I2C ACK at 0x43. Antenna switch off and GPS unpowered."),
              F("FATAL: IO expander"));
    }
    Serial.println(F("IO expander: P0 high (antenna switch + GPS power)."));
    splashLine(F("IO expander: OK"));

    if (!spiBusInit()) {
        fatal(F("FATAL: could not create the SPI bus mutex."), F("FATAL: SPI bus mutex"));
    }

    // One-shot channel override, before the radio starts. Sequential with
    // everything else here — no tasks are running yet, so no arbitration is
    // needed for this read.
    bool sdOverrideApplied = loadChannelConfigFromSD(activeChannel, PIN_SD_CS, sharedSpi());
    splashLine(sdOverrideApplied ? F("Config: SD override") : F("Config: default"));

    detectionQueue = xQueueCreate(DETECTION_QUEUE_DEPTH, sizeof(Detection));
    if (detectionQueue == nullptr) {
        fatal(F("FATAL: could not allocate the detection queue."), F("FATAL: queue alloc"));
    }

    // Consumers before producer: the logger must be draining before the
    // radio starts filling, or the first burst is dropped for no reason.
    if (!gpsTaskStart()) {
        Serial.println(F("WARN: GPS task failed to start — detections will log without position."));
        splashLine(F("GPS task: FAILED"), SPLASH_ERR);
    } else {
        splashLine(F("GPS task: started"));
    }

    if (!loggerTaskStart(detectionQueue)) {
        fatal(F("FATAL: logger task failed to start."), F("FATAL: logger task"));
    }
    splashLine(F("Logger task: started"));

    if (!radioTaskStart(activeChannel, detectionQueue)) {
        Serial.print(F("FATAL: radio start failed, RadioLib code "));
        Serial.println(radioLastError());
        splashLine("FATAL: radio " + String(radioLastError()), SPLASH_ERR);
        while (true) delay(1000);
    }

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

    Serial.println(F("Radio task listening on Core 1."));

    Serial.print(F("Free heap after task start: "));
    Serial.print(ESP.getFreeHeap());
    Serial.println(F(" bytes"));

    // UI last: it takes ownership of the display, so everything above gets
    // to use the splash for boot progress first. From here main.cpp must
    // never touch `tft` again — two writers on one panel is a race with no
    // upside.
    batteryInit();
    if (!uiTaskStart(tft)) {
        // Non-fatal on purpose: a headless wardriver still logs, which is
        // the actual job. Serial keeps reporting either way.
        Serial.println(F("WARN: UI task failed to start — continuing headless."));
    } else if (!uiKeyboardReady()) {
        Serial.println(F("WARN: TCA8418 keyboard not detected — UI pages will auto-advance."));
    }

    Serial.println(F("To return to Launcher: press any key during its ~5s boot window, or enable its Settings -> \"Boot to Launcher\" toggle."));
}

void loop() {
    // No display work here any more: ui_task owns the panel once started
    // (including its own liveness indication), so the old heartbeat dot and
    // inline status rows would race it for the same pixels.
    static uint32_t lastStatus = 0;
    uint32_t now = millis();
    if (now - lastStatus < 5000) {
        delay(20); // nothing to do; leave the CPU to the tasks
        return;
    }
    lastStatus = now;

    GpsFix fix;
    bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(100));

    // One line carrying everything Phase 2's exit criteria need: packets in,
    // rows on the card, drops (which must stay at zero), and the worst bus
    // hold the logger has caused — the number that says whether batching is
    // starving the radio.
    Serial.print(F("[status] rx="));
    Serial.print(radioPacketCount());
    Serial.print(F(" crcerr="));
    Serial.print(radioCrcErrorCount());
    Serial.print(F(" qdrop="));
    Serial.print(radioQueueDropCount());
    Serial.print(F(" busmiss="));
    Serial.print(radioBusMissCount());
    Serial.print(F(" | rows="));
    Serial.print(loggerRowsWritten());
    Serial.print(F(" rowdrop="));
    Serial.print(loggerRowsDropped());
    Serial.print(F(" flushes="));
    Serial.print(loggerFlushCount());
    Serial.print(F(" maxflush="));
    Serial.print(loggerMaxFlushMs());
    // Separate from maxflush on purpose: the health writer's bus holds are
    // instrumentation, and folding them in makes the flush number lie about
    // batch sizing (first hardware run printed flushes=0 maxflush=26ms).
    Serial.print(F("ms maxhealth="));
    Serial.print(loggerMaxSessionMs());
    Serial.print(F("ms sd="));
    Serial.print(loggerSdReady() ? F("ok") : F("DOWN"));
    Serial.print(F(" health="));
    Serial.print(loggerSessionRows());
    Serial.print(F(" run="));
    Serial.print(loggerRunIndex());
    Serial.print(F(" | nmea="));
    Serial.print(gpsSentenceCount());
    Serial.print(F(" badcrc="));
    Serial.print(gpsChecksumErrorCount());
    Serial.print(F(" fix="));
    if (haveFix && fix.has_position) {
        Serial.print(fix.lat, 6);
        Serial.print(',');
        Serial.print(fix.lon, 6);
        Serial.print(F(" sats="));
        Serial.print(fix.satellites);
    } else {
        Serial.print(F("none"));
    }
    Serial.print(F(" | heap="));
    Serial.print(ESP.getFreeHeap());
    // The trough, not just the sample: a 5s status line can walk right past
    // a transient dip, and the dip is what actually ends a long run.
    Serial.print(F(" heapmin="));
    Serial.println(ESP.getMinFreeHeap());
}
