// LoRaTrace RX — Phase 2 (task/queue architecture, GPS, SD logging) plus
// Phase 3's WiFi AP/web UI (ui_task's on-device pages arrived early — see
// ui_task.h) and Phase 4's MeshCore profile: this file still only ever boots
// radio_task on Meshtastic (below), and the live switch to/from MeshCore is
// entirely ui_task's/radio_task's own affair from there (DESIGN.md §5). This
// file is an orchestrator, not a driver: it brings up hardware in a fixed
// order, then hands the work to five tasks and gets out of the way.
//
//   Core 1: radio_task   — owns the SX1262, HOME_LISTEN, never blocks
//   Core 0: gps_task     — NMEA -> last-fix behind a mutex
//   Core 0: logger_task  — dequeue, GPS-stamp, batched SD writes
//   Core 0: ui_task      — on-device display pages + keyboard
//   Core 0: wifi_task    — AP + web UI, off until toggled (lowest priority)
//
// Detections cross cores through a FreeRTOS queue as ~36-byte structs
// (detection.h). SD and the SX1262 share one physical SPI bus, so every
// task that touches SD arbitrates through spi_bus.h — the queue alone is
// not enough, since two devices on one bus cannot transact at the same
// instant no matter which core issues them.
//
// loop() keeps only what genuinely belongs on the main task: a periodic
// Serial status line. Nothing here touches the display — ui_task owns it
// exclusively once started (see the ordering note before uiTaskStart()
// below).
//
// Boot order matters and is not arbitrary:
//   1. NSS high      — before any I2C/SPI, or SD mounts unreliably
//   2. IO expander   — antenna switch AND GPS power (io_expander.h)
//   3. SPI bus       — mutex + peripheral, before anything touches it
//   4. SD config     — one-shot channel override, before the radio starts
//   5. Tasks         — radio last, so the queue exists before packets do

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "backlight.h"
#include "battery.h"
#include "board_pins.h"
#include "channel_plans.h"
#include "config.h"
#include "detection.h"
#include "display_settings.h"
#include "gps_task.h"
#include "io_expander.h"
#include "logger_task.h"
#include "radio_task.h"
#include "serial_lock.h"
#include "spi_bus.h"
#include "ui_task.h"
#include "version.h"
#include "wifi_task.h"

// Boot-status splash — PROGRESS.md decisions log: a narrow, deliberate
// exception to CLAUDE.md's "no UI yet," not ui_task's own redraw loop. Own SPI
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
// `channelOverrides` holds BOTH profiles' overrides (per-profile since
// 2026-08-24, see PROGRESS.md Decisions log); `activeChannel` below is just
// the boot profile's resolved value, handed to radioTaskStart() alongside
// `channelOverrides` itself so a later profile switch can resolve MeshCore's
// override too, not just Meshtastic's.
ProfileOverrides channelOverrides;
ChannelParams activeChannel = CHANNEL_MESHTASTIC_LONGFAST_US;
DisplaySettings displaySettings;

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
    backlightInit(); // LEDC PWM, starts at 100% — see backlight.h
    if (!tft->begin()) return false;
    tft->fillScreen(SPLASH_BG);
    tft->setTextSize(1);
    return true;
}

// The boot-time liveness heartbeat now lives in ui_task (it owns the panel
// after setup). Keeping a copy here would race it for the same pixels.

void fatal(const __FlashStringHelper *serialMsg, const __FlashStringHelper *splashMsg) {
    {
        // Some fatal() call sites run after gpsTaskStart()/loggerTaskStart(),
        // so this can race a Core 0 task's own print — see serial_lock.h.
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) Serial.println(serialMsg);
    }
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

    // Before any task is started, so every later print (this function's own
    // included) has it available — see serial_lock.h.
    if (!serialLockInit()) {
        // No Serial lock to report through — this is the one message in
        // the whole file that can't route through itself. Best effort only.
        Serial.println(F("FATAL: could not create the Serial mutex."));
        while (true) delay(1000);
    }

    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.print(F("LoRaTrace RX v"));
            Serial.print(FIRMWARE_VERSION);
            Serial.print(F(" ("));
            Serial.print(FIRMWARE_BUILD_REV); // git SHA — identifies THIS binary
            Serial.println(F(") — tasks + GPS + SD logging + WiFi + MeshCore + on-device menu"));
        }
    }

    displayReady = initDisplay();
    tft->setTextSize(2);
    splashLine(F("LoRaTrace RX"));
    splashY += SPLASH_LINE_H;
    tft->setTextSize(1);
    splashLine(String("v") + FIRMWARE_VERSION);
    splashY += SPLASH_LINE_H / 2;

    // P0 high: RF antenna switch AND GPS power. Fatal because both halves
    // of this device's job depend on it.
    if (!ioExpanderInit()) {
        fatal(F("FATAL: IO expander init failed — no I2C ACK at 0x43. Antenna switch off and GPS unpowered."),
              F("FATAL: IO expander"));
    }
    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) Serial.println(F("IO expander: P0 high (antenna switch + GPS power)."));
    }
    splashLine(F("IO expander: OK"));

    if (!spiBusInit()) {
        fatal(F("FATAL: could not create the SPI bus mutex."), F("FATAL: SPI bus mutex"));
    }

    // One-shot channel override, before the radio starts. Sequential with
    // everything else here — no tasks are running yet, so no arbitration is
    // needed for this read.
    loadProfileOverridesFromSD(channelOverrides, PIN_SD_CS, sharedSpi());
    activeChannel = resolvedChannelForProfile(channelOverrides, MissionProfile::MESHTASTIC);
    // Card is already mounted by the call above (or there's no card, in
    // which case this fails safe the same way) — no separate SD.begin().
    loadDisplaySettingsFromSD(displaySettings);
    // Specifically whether *this boot's profile* (Meshtastic) has an
    // override, not whether the file had any override at all — a
    // MeshCore-only override would otherwise make this splash line claim
    // "SD override" while the channel actually in use is still the
    // hardcoded Meshtastic default.
    splashLine(channelOverrides.meshtastic_set ? F("Config: SD override") : F("Config: default"));

    detectionQueue = xQueueCreate(DETECTION_QUEUE_DEPTH, sizeof(Detection));
    if (detectionQueue == nullptr) {
        fatal(F("FATAL: could not allocate the detection queue."), F("FATAL: queue alloc"));
    }

    // Consumers before producer: the logger must be draining before the
    // radio starts filling, or the first burst is dropped for no reason.
    if (!gpsTaskStart()) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.println(F("WARN: GPS task failed to start — detections will log without position."));
        }
        splashLine(F("GPS task: FAILED"), SPLASH_ERR);
    } else {
        splashLine(F("GPS task: started"));
    }

    if (!loggerTaskStart(detectionQueue)) {
        fatal(F("FATAL: logger task failed to start."), F("FATAL: logger task"));
    }
    splashLine(F("Logger task: started"));

    // Boots on Meshtastic; MeshCore is reachable at runtime via ui_task's
    // menu (DESIGN.md §5, radio_task.h). `channelOverrides` is passed along
    // too, not just this boot profile's already-resolved `activeChannel` —
    // radio_task.cpp holds onto it so a later switch to MeshCore resolves
    // *its* override (if any) the same way this boot resolved Meshtastic's,
    // rather than always falling back to channel_plans.h's hardcoded table.
    if (!radioTaskStart(activeChannel, MissionProfile::MESHTASTIC, channelOverrides, detectionQueue)) {
        {
            SerialLock lock(pdMS_TO_TICKS(200));
            if (lock.held()) {
                Serial.print(F("FATAL: radio start failed, RadioLib code "));
                Serial.println(radioLastError());
            }
        }
        splashLine("FATAL: radio " + String(radioLastError()), SPLASH_ERR);
        while (true) delay(1000);
    }

    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
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
        }
    }
    splashLine(String(activeChannel.freq_mhz, 3) + "MHz SF" + activeChannel.sf);
    splashLine("BW" + String(activeChannel.bw_khz, 1) + " CR4/" + activeChannel.cr_denom +
               " sync 0x" + String(activeChannel.sync_word, HEX));

    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.println(F("Radio task listening on Core 1."));
            // "` to open/close" — the ESC/backtick key, not literally the
            // ASCII backtick as a menu action; matches the on-device menu's
            // own footer hint (ui_task.cpp) after the 2026-08-24 rework that
            // moved menu-open off Enter and onto this key.
            Serial.println(F("On-device menu: ,/. to move, ` to open/close, Enter to act "
                             "-- covers profile switch (Meshtastic/MeshCore, DESIGN.md S5), "
                             "WiFi, and verbose debug logging."));
            Serial.print(F("Free heap after task start: "));
            Serial.print(ESP.getFreeHeap());
            Serial.println(F(" bytes"));
        }
    }

    // Off until toggled (ui_task's on-device menu, Phase 5) — starting the
    // task itself is cheap, starting the AP is what actually costs RAM/CPU/
    // RF noise, so that stays deferred until an operator asks for it.
    // Started (and its splash line drawn) before uiTaskStart() below, same
    // reason everything else in setup() draws its own splash line before
    // that point: this is the last call allowed to touch `tft` directly.
    if (!wifiTaskStart()) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.println(F("WARN: WiFi task failed to start — web UI unavailable this run."));
        }
    } else {
        char ssid[32];
        wifiApSsid(ssid, sizeof(ssid));
        {
            SerialLock lock(pdMS_TO_TICKS(200));
            if (lock.held()) {
                Serial.print(F("WiFi: use the on-device menu to enable AP '"));
                Serial.print(ssid);
                Serial.println(F("'."));
            }
        }
        splashLine("WiFi: menu -> " + String(ssid));
    }

    // UI last: it takes ownership of the display, so everything above gets
    // to use the splash for boot progress first. From here main.cpp must
    // never touch `tft` again — two writers on one panel is a race with no
    // upside.
    batteryInit();
    if (!uiTaskStart(tft, displaySettings)) {
        // Non-fatal on purpose: a headless wardriver still logs, which is
        // the actual job. Serial keeps reporting either way.
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) Serial.println(F("WARN: UI task failed to start — continuing headless."));
    } else if (!uiKeyboardReady()) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.println(F("WARN: TCA8418 keyboard not detected — UI pages will auto-advance."));
        }
    }
    // The Launcher-return hint that used to print here is gone: it is static
    // documentation, identical on every boot, and it belongs in the README's
    // M5Launcher section (where it already is) rather than in a log an
    // operator scans for what this particular run is doing.
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

    char fixStr[48];
    if (haveFix && fix.has_position) {
        snprintf(fixStr, sizeof(fixStr), "%.6f,%.6f sats=%u", fix.lat, fix.lon, (unsigned)fix.satellites);
    } else {
        snprintf(fixStr, sizeof(fixStr), "none");
    }

    // One line carrying everything Phase 2's exit criteria need: packets in,
    // rows on the card, drops (which must stay at zero), and the worst bus
    // hold the logger has caused — the number that says whether batching is
    // starving the radio.
    //
    // Built into one buffer, then printed under serialLockTake() below —
    // this task's own loop() very likely runs on Core 1 (Arduino's
    // default), the same core as radio_task, while wifi_task/logger_task/
    // gps_task run on Core 0, and this is the project's primary diagnostic
    // line. One buffer alone was the original (2026-08-23) fix, on the
    // theory that a single Serial call is far more likely to be atomic
    // than several; a 2026-08-24 session proved a single call can still be
    // torn by another core's call landing mid-write, so the lock (added
    // that session, see serial_lock.h) is what actually protects this now
    // — the single buffer just keeps the critical section short.
    char line[384];
    int n = snprintf(line, sizeof(line),
                     "[status] rx=%lu crcerr=%lu qdrop=%lu busmiss=%lu | rows=%lu rowdrop=%lu "
                     "flushes=%lu maxflush=%lums maxhealth=%lums sd=%s health=%lu run=%u | "
                     "nmea=%lu badcrc=%lu fix=%s | heap=%lu heapmin=%lu",
                     (unsigned long)radioPacketCount(), (unsigned long)radioCrcErrorCount(),
                     (unsigned long)radioQueueDropCount(), (unsigned long)radioBusMissCount(),
                     (unsigned long)loggerRowsWritten(), (unsigned long)loggerRowsDropped(),
                     (unsigned long)loggerFlushCount(), (unsigned long)loggerMaxFlushMs(),
                     // Separate from maxflush on purpose: the health writer's
                     // bus holds are instrumentation, and folding them in
                     // makes the flush number lie about batch sizing (first
                     // hardware run printed flushes=0 maxflush=26ms).
                     (unsigned long)loggerMaxSessionMs(), loggerSdReady() ? "ok" : "DOWN",
                     (unsigned long)loggerSessionRows(), (unsigned)loggerRunIndex(),
                     (unsigned long)gpsSentenceCount(), (unsigned long)gpsChecksumErrorCount(), fixStr,
                     // The trough, not just the sample: a 5s status line can
                     // walk right past a transient dip, and the dip is what
                     // actually ends a long run.
                     (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
    if (n > 0) {
        // One buffer, one call was the 2026-08-23 fix; this lock is the
        // 2026-08-24 one — see serial_lock.h for why the first alone wasn't
        // enough. A held-but-lost race here just means this cycle's line is
        // skipped rather than torn; the next one is 5s away.
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) Serial.println(line);
    }
}
