// LoRaTrace RX — orchestrator, not a driver: brings up hardware in a fixed
// order, then hands off to five tasks and gets out of the way. Boots
// radio_task on Meshtastic; the live switch to/from MeshCore is entirely
// ui_task's/radio_task's own affair from there (docs/DESIGN.md §5).
//
//   Core 1: radio_task   — owns the SX1262, HOME_LISTEN, never blocks
//   Core 0: gps_task     — NMEA -> last-fix behind a mutex
//   Core 0: logger_task  — dequeue, GPS-stamp, batched SD writes
//   Core 0: ui_task      — on-device display pages + keyboard
//   Core 0: wifi_task    — AP + web UI, off until toggled (lowest priority)
//
// Detections cross cores through a FreeRTOS queue as ~36-byte structs
// (detection.h). SD and the SX1262 share one physical SPI bus, so every
// task touching SD arbitrates through spi_bus.h — two devices on one bus
// can't transact at the same instant no matter which core issues them.
//
// loop() only keeps the optional Debug health line — ui_task owns the display
// exclusively once started.
//
// Boot order matters:
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
#include "serial_control.h"
#include "profile_state.h"
#include "radio_task.h"
#include "serial_lock.h"
#include "spi_bus.h"
#include "ui_task.h"
#include "version.h"
#include "wifi_task.h"

// Boot-status splash — a deliberate one-off, not ui_task's redraw loop.
// Own SPI host (HSPI), disjoint from the radio/SD bus (board_pins.h), so
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
// splashX starts at the old flush-left margin so a FATAL firing before
// playBootMark() runs (or if display init itself failed) still reads
// exactly as it always has — playBootMark() moves it in once the mark has
// actually drawn, not before.
int16_t splashX = 4;
constexpr uint16_t SPLASH_BG = 0x0000;    // RGB565 black
constexpr uint16_t SPLASH_FG = 0xFFFF;    // RGB565 white
constexpr uint16_t SPLASH_ERR = 0xF800;   // RGB565 red
constexpr uint8_t SPLASH_LINE_H = 10;     // px, text size 1
// Boot-mark accent colours (2026-08-25, operator-approved concept: see the
// "Signal Acquired" mockup). Deliberately distinct from every accent
// already in use on-device — SPLASH_GREEN isn't ui_pages.cpp's COL_GOOD,
// and SPLASH_AMBER isn't its COL_WARN — this mark is a one-shot boot
// moment, not a status colour, and shouldn't borrow meaning from either.
// Muted sage green / warm amber-gold, quantized to RGB565 from #4aa273 /
// #deb221 respectively.
constexpr uint16_t SPLASH_GREEN = 0x4D0E;
constexpr uint16_t SPLASH_AMBER = 0xDD84;

// Meshtastic LongFast (US) unless overridden by /loratrace/config.txt on
// SD — see config.h/config.cpp and sd-template/loratrace/config.txt.
// `channelOverrides` holds BOTH profiles' overrides (per-profile since
// 2026-08-24, see docs/history/CHANGELOG.md); `activeChannel` below is just
// the boot profile's resolved value, handed to radioTaskStart() alongside
// `channelOverrides` itself so a later profile switch can resolve MeshCore's
// override too, not just Meshtastic's.
ProfileOverrides channelOverrides;
ChannelParams activeChannel = CHANNEL_MESHTASTIC_LONGFAST_US;
DisplaySettings displaySettings;

// Detection queue, Core 1 -> Core 0. Each entry includes one bounded raw
// frame, so 16 entries use roughly 4.8KB. That still rides out normal SD
// flushes while keeping the no-PSRAM allocation intentional; watch
// radioQueueDropCount() in the status line before changing it.
constexpr UBaseType_t DETECTION_QUEUE_DEPTH = 16;
constexpr UBaseType_t NODE_IDENTITY_QUEUE_DEPTH = 16;
constexpr UBaseType_t SCAN_OBSERVATION_QUEUE_DEPTH = 16;
// A noisy environment can produce far more Sweep peaks per run than
// Probe's ~9-candidate CAD sweep ever could — a starting choice, not a
// measured one, same as SCAN_OBSERVATION_QUEUE_DEPTH above.
constexpr UBaseType_t ENERGY_OBSERVATION_QUEUE_DEPTH = 32;
// How long the completed boot checklist stays on screen before uiTaskStart()
// takes over the panel with the main status pages.
constexpr uint32_t BOOT_CHECKLIST_HOLD_MS = 1000;
QueueHandle_t detectionQueue = nullptr;
QueueHandle_t scanObservationQueue = nullptr;
QueueHandle_t energyObservationQueue = nullptr;
QueueHandle_t identityQueue = nullptr;

void splashLine(const String &msg, uint16_t color = SPLASH_FG) {
    if (!displayReady) return;
    tft->setTextColor(color, SPLASH_BG);
    tft->setCursor(splashX, splashY);
    tft->println(msg);
    splashY += SPLASH_LINE_H;
}

// Boot mark (2026-08-25) — docs/BRAND.md's unbuilt logo concept: an L-shaped
// path resolving into three signal arcs. Procedural (drawLine/fillArc,
// a handful of coordinate constants), not a bitmap. No alpha blending
// (RGB565 has none) — motion is hard colour swaps and staged reveals.
// Sourced from the approved preview (see docs/history/CHANGELOG.md for the mockup
// link and full round-by-round history), rescaled to this real budget.
//
// **Angle convention:** fillArc() measures 0°=3 o'clock, clockwise — the
// same convention as HTML canvas, so mockup angles need no conversion.
// An earlier +90° "conversion" was a bug, not a fix (rotated the mark 90°,
// see docs/history/CHANGELOG.md v0.6.6) — removed.
constexpr int16_t MARK_ANCHOR_X = 30;
constexpr int16_t MARK_ANCHOR_Y = 28;
// Diagonal-foot path (round 5, the shipped pick over a straight run
// compared alongside it in the mockup): a short kick left just before
// the bottom edge, like a foot anchoring the line, straight up to an
// elbow just below the mark, then into the anchor.
constexpr int16_t MARK_PATH_X0 = 6, MARK_PATH_Y0 = 130;  // path start, near the bottom edge
constexpr int16_t MARK_PATH_X1 = 14, MARK_PATH_Y1 = 108; // foot kick
constexpr int16_t MARK_PATH_X2 = 14, MARK_PATH_Y2 = 32;  // elbow, just below the mark
// path then runs elbow -> anchor, completing the shape.
constexpr int16_t MARK_ARC_RADII[3] = {9, 15, 21};
constexpr int16_t MARK_ARC_THICKNESS = 2;
constexpr float MARK_ARC_START_DEG = -44.7f; // canvas angle, unconverted — see note above
constexpr float MARK_ARC_END_DEG = 44.1f;    // canvas angle, unconverted — see note above
// Wordmark/version sit beside the mark, right of its outermost arc.
constexpr int16_t MARK_WORD_X = MARK_ANCHOR_X + MARK_ARC_RADII[2] + 8; // = 59
constexpr int16_t MARK_WORD_Y = 20;
constexpr int16_t MARK_VERSION_Y = 38;
// Where the diagnostic log picks up: aligned under the first (innermost)
// arc's rightmost edge, per direct operator feedback on the mockup ("fall
// in line with the first signal arc") rather than the old flush-left x=4.
constexpr int16_t MARK_LOG_X = MARK_ANCHOR_X + MARK_ARC_RADII[0]; // = 39
constexpr int16_t MARK_LOG_Y = 66;

// Signal-trace flourish: a fixed (not random) jagged pattern stepped
// through a few discrete frames, filling the panel's lower third. A fixed
// pattern reads as a consistent "signal" frame to frame rather than noise.
// Ambient, not a progress bar — plays before the real checklist lines
// below even print, so it never claims to track their progress.
constexpr int16_t TRACE_PATTERN[12] = {0, -3, 2, -6, 5, -2, 4, -5, 1, -4, 3, -1};
constexpr uint8_t TRACE_PATTERN_LEN = 12;
constexpr int16_t TRACE_Y = 112;
constexpr int16_t TRACE_AMP = 9;
constexpr int16_t TRACE_X0 = MARK_LOG_X; // = 39, anchored under the log column
constexpr int16_t TRACE_X1 = 236;
constexpr uint8_t TRACE_FRAMES = 7;
constexpr uint16_t TRACE_FRAME_MS = 110;
// Dim baseline under the trace, quantized from #2d5940 — the same muted
// green family as SPLASH_GREEN but visually receded, giving the trace a
// reference line to read against (same "show the axis" instinct as
// ui_pages.cpp's drawFreqBar() track on the real CHANNEL page).
constexpr uint16_t SPLASH_GREEN_DIM = 0x2AC8;

void drawSignalTraceFrame(uint8_t frame) {
    // Band clear, not a full-panel redraw — avoids trailing garbage from
    // the previous frame while leaving the mark/wordmark above untouched.
    // Clipped to [TRACE_X0, TRACE_X1), the exact x-range the trace itself
    // draws in — NOT the full panel width (bug found 2026-08-25, v0.6.7):
    // the diagonal-foot path segment and the pole's own tail (drawn once,
    // earlier in playBootMark(), at x < TRACE_X0) physically sit inside
    // this same y-band, so a full-width clear here wiped a little more of
    // them on every one of the 7 animation frames, and nothing ever
    // redrew them — left a solid black gap where the lower pole/foot used
    // to be, with only the foot's tail (below the band) surviving.
    tft->fillRect(TRACE_X0, TRACE_Y - TRACE_AMP - 2, TRACE_X1 - TRACE_X0, TRACE_AMP * 2 + 4, SPLASH_BG);

    int16_t prevX = TRACE_X0;
    int16_t prevY = TRACE_Y + TRACE_PATTERN[frame % TRACE_PATTERN_LEN];
    for (uint8_t i = 1; i <= TRACE_PATTERN_LEN; i++) {
        const int16_t x = TRACE_X0 + (int32_t)(TRACE_X1 - TRACE_X0) * i / TRACE_PATTERN_LEN;
        const int16_t y = TRACE_Y + TRACE_PATTERN[(i + frame) % TRACE_PATTERN_LEN];
        tft->drawLine(prevX, prevY, x, y, SPLASH_GREEN);
        prevX = x;
        prevY = y;
    }
    tft->drawLine(TRACE_X0, TRACE_Y + TRACE_AMP + 6, TRACE_X1, TRACE_Y + TRACE_AMP + 6, SPLASH_GREEN_DIM);
}

void playBootMark() {
    if (!displayReady) return;

    // Diagonal-foot L-path, drawn as three connected segments rather than
    // one so it reads as traced rather than stamped — cheap motion with
    // no per-pixel interpolation.
    tft->drawLine(MARK_PATH_X0, MARK_PATH_Y0, MARK_PATH_X1, MARK_PATH_Y1, SPLASH_GREEN);
    delay(90);
    tft->drawLine(MARK_PATH_X1, MARK_PATH_Y1, MARK_PATH_X2, MARK_PATH_Y2, SPLASH_GREEN);
    delay(90);
    tft->drawLine(MARK_PATH_X2, MARK_PATH_Y2, MARK_ANCHOR_X, MARK_ANCHOR_Y, SPLASH_GREEN);
    tft->fillCircle(MARK_ANCHOR_X, MARK_ANCHOR_Y, 1, SPLASH_GREEN);
    delay(220);

    // Three arcs, sequential "acquire" (Take B of the mockup, the
    // operator's pick over the simultaneous "radar ping" take): each
    // lands amber, holds briefly, then locks over to green — a hard
    // colour swap, not a fade, since RGB565 has no alpha to fade through.
    for (uint8_t i = 0; i < 3; i++) {
        const int16_t r = MARK_ARC_RADII[i];
        tft->fillArc(MARK_ANCHOR_X, MARK_ANCHOR_Y, r, r - MARK_ARC_THICKNESS,
                     MARK_ARC_START_DEG, MARK_ARC_END_DEG, SPLASH_AMBER);
        delay(140);
        tft->fillArc(MARK_ANCHOR_X, MARK_ANCHOR_Y, r, r - MARK_ARC_THICKNESS,
                     MARK_ARC_START_DEG, MARK_ARC_END_DEG, SPLASH_GREEN);
        delay(i == 2 ? 200 : 120);
    }

    // Wordmark, one line, kept together per round 5's direct feedback
    // (round 4 briefly split "LoRaTrace"/"RX" across two lines to hit
    // size 3 — reversed the same day). Two consecutive print() calls need
    // no manual width math the way the mockup's canvas preview did (see
    // the function-level comment above) — "RX" just continues from
    // wherever "LoRaTrace " actually left the cursor.
    tft->setTextSize(2);
    tft->setCursor(MARK_WORD_X, MARK_WORD_Y);
    tft->setTextColor(SPLASH_FG, SPLASH_BG);
    tft->print(F("LoRaTrace "));
    tft->setTextColor(SPLASH_GREEN, SPLASH_BG);
    tft->print(F("RX"));
    delay(150);

    tft->setTextSize(1);
    tft->setTextColor(SPLASH_GREEN, SPLASH_BG);
    tft->setCursor(MARK_WORD_X, MARK_VERSION_Y);
    tft->print(String("v") + FIRMWARE_VERSION);
    delay(250);

    // Signal-trace flourish: a handful of discrete frames, then holds on
    // its last frame — see the constants/comment above for why this
    // plays here rather than trailing the real checklist lines below.
    for (uint8_t frame = 0; frame < TRACE_FRAMES; frame++) {
        drawSignalTraceFrame(frame);
        delay(TRACE_FRAME_MS);
    }

    // Hand off to splashLine()'s log sequence, aligned under the first arc
    // instead of the old x=4.
    splashX = MARK_LOG_X;
    splashY = MARK_LOG_Y;
}

bool initDisplay() {
    tftSPI.begin(PIN_TFT_SCLK, -1 /* MISO unused */, PIN_TFT_MOSI, PIN_TFT_CS);
    if (!tft->begin()) return false;
    tft->fillScreen(SPLASH_BG);
    tft->setTextSize(1);
    // Backlight comes up AFTER the panel is cleared, not before — the
    // ST7789's GRAM survives a warm reset, so a backlight-first order lit
    // up ui_task's stale last-drawn frame for the ~120ms+ the panel takes
    // to wake (the "flashes the old page, then the boot mark" symptom).
    backlightInit();
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
    // board_pins.h and docs/history/PROGRESS.md.
    pinMode(PIN_LORA_NSS, OUTPUT);
    digitalWrite(PIN_LORA_NSS, HIGH);

    // The ESP32-S3 native USB-CDC default TX ring is only 256 bytes.  Phase 7
    // diagnostics can emit several 100+ byte lines back-to-back while WiFi
    // starts, so give the driver enough queueing room before begin() creates
    // its default ring.  This is a small, fixed allocation, not per-message
    // heap churn.
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxBufferSize(1024);
#endif
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
    playBootMark();

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
    bool sdMounted = false;
    loadProfileOverridesFromSD(channelOverrides, PIN_SD_CS, sharedSpi(), &sdMounted);
    // Last profile an operator actually selected via the menu, not always
    // Meshtastic — see profile_state.h for why this used to be hardcoded.
    MissionProfile bootProfile = MissionProfile::MESHTASTIC;
    loadLastProfileFromSD(bootProfile);
    activeChannel = resolvedChannelForProfile(channelOverrides, bootProfile);
    // Card is already mounted by the call above (or there's no card, in
    // which case this fails safe the same way) — no separate SD.begin().
    loadDisplaySettingsFromSD(displaySettings);
    // A mounted card with no override file is a fine, expected state, not
    // a checklist failure — this is SD.begin()'s own result, not "was a
    // config applied" (that detail is still in serial/the CHANNEL page).
    splashLine(sdMounted ? F("SD: OK") : F("SD: MISSING"), sdMounted ? SPLASH_FG : SPLASH_ERR);

    detectionQueue = xQueueCreate(DETECTION_QUEUE_DEPTH, sizeof(Detection));
    if (detectionQueue == nullptr) {
        fatal(F("FATAL: could not allocate the detection queue."), F("FATAL: queue alloc"));
    }
    scanObservationQueue = xQueueCreate(SCAN_OBSERVATION_QUEUE_DEPTH, sizeof(ScanObservation));
    if (scanObservationQueue == nullptr) {
        fatal(F("FATAL: could not allocate the scan queue."), F("FATAL: scan queue alloc"));
    }
    energyObservationQueue = xQueueCreate(ENERGY_OBSERVATION_QUEUE_DEPTH, sizeof(EnergyObservation));
    if (energyObservationQueue == nullptr) {
        fatal(F("FATAL: could not allocate the energy queue."), F("FATAL: energy queue alloc"));
    }
    identityQueue = xQueueCreate(NODE_IDENTITY_QUEUE_DEPTH, sizeof(NodeIdentity));
    if (identityQueue == nullptr) {
        fatal(F("FATAL: could not allocate the identity queue."), F("FATAL: identity queue alloc"));
    }

    // Consumers before producer: the logger must be draining before the
    // radio starts filling, or the first burst is dropped for no reason.
    // No splash line: gpsTaskStart() only spawns a task + mutex, it never
    // talks to the GPS module, so "GPS: OK" here would check nothing —
    // real fix status is a glance away on the GPS page moments after boot.
    if (!gpsTaskStart()) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.println(F("WARN: GPS task failed to start — detections will log without position."));
        }
    }

    if (!loggerTaskStart(detectionQueue, scanObservationQueue, energyObservationQueue, identityQueue,
                         sdMounted)) {
        fatal(F("FATAL: logger task failed to start."), F("FATAL: logger task"));
    }
    // No splash line on success: this is RTOS resource allocation, not a
    // hardware check, and its only failure mode is already fatal() above.

    // Boots on the last profile selected via the menu (defaults to
    // Meshtastic on a first boot / no SD); the other profile is reachable
    // at runtime via ui_task's menu (docs/DESIGN.md §5, radio_task.h).
    // `channelOverrides` is passed too, not just `activeChannel` —
    // radio_task.cpp holds onto it so a later switch resolves *its*
    // override the same way this boot resolved `bootProfile`'s.
    if (!radioTaskStart(activeChannel, bootProfile, channelOverrides, detectionQueue,
                        scanObservationQueue, energyObservationQueue, identityQueue)) {
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
    // radio.begin() above is a real SPI transaction with the SX1262 (see
    // radio_task.cpp), so this line — unlike the GPS/logger lines removed
    // above — is a genuine hardware check, not just "a task started."
    splashLine(F("Radio: OK"));
    // Hold the completed checklist on screen briefly before uiTaskStart()
    // repaints the panel with the main status pages — otherwise "Radio: OK"
    // is visible for only a frame or two.
    delay(BOOT_CHECKLIST_HOLD_MS);

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
    // No splash line for the channel detail — it's already in serial
    // above and on the CHANNEL page moments after boot; showing it here
    // too was often just the hardcoded default and read as more meaningful
    // than it was.

    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            Serial.println(F("Radio task listening on Core 1."));
            // "` to open/close" — the ESC/backtick key, not literally the
            // ASCII backtick as a menu action; matches the on-device menu's
            // own footer hint (ui_task.cpp) after the 2026-08-24 rework that
            // moved menu-open off Enter and onto this key.
            Serial.println(F("On-device menu: ,/. to move, ` to open/close, Enter to act "
                             "-- Trace, profile switch (Meshtastic/MeshCore, docs/DESIGN.md S5), "
                             "WiFi, and verbose debug logging; P runs Probe anywhere. "
                             "Enter toggles Trace on Radio or Probe on its card."));
            Serial.print(F("Free heap after task start: "));
            Serial.print(ESP.getFreeHeap());
            Serial.println(F(" bytes"));
        }
    }

    // Off until toggled (ui_task's on-device menu) — starting the task is
    // cheap, starting the AP is what costs RAM/CPU/RF noise, so that stays
    // deferred until an operator asks for it. Started before uiTaskStart()
    // below since this is the last call allowed to touch `tft` directly.
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
        // No splash line — the SSID is what the WiFi-toggle toast already
        // shows the moment an operator turns the AP on (ui_actions.cpp),
        // which is the point they need it, not before.
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
    // No Launcher-return hint here: that's static, identical-every-boot
    // documentation and belongs in the README, not a log an operator scans
    // for what this particular run is doing.
}

void loop() {
    // No display work here any more: ui_task owns the panel once started
    // (including its own liveness indication), so the old heartbeat dot and
    // inline status rows would race it for the same pixels.
    static uint32_t lastStatus = 0;
    uint32_t now = millis();
    // Serial Control owns the USB console while enabled. Suppress the
    // human-readable health line there, and keep it Debug-gated otherwise so
    // an unattended device does not spend CPU/USB time on periodic noise.
    if (serialControlIsEnabled() || !loggerDebugIsEnabled()) {
        delay(20);
        return;
    }
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

    // One line carrying Phase 2's exit-criteria numbers: packets in, rows
    // written, drops (must stay 0), worst logger-caused bus hold. Built
    // into one buffer, then printed under the Serial lock below — a single
    // buffer alone isn't enough to stop tearing across cores (found
    // 2026-08-24), the lock is what actually protects it (serial_lock.h).
    char line[384];
    int n = snprintf(line, sizeof(line),
                     "[status] rx=%lu crcerr=%lu qdrop=%lu busmiss=%lu | rows=%lu rowdrop=%lu "
                     "nodes=%lu nodedrop=%lu "
                     "flushes=%lu maxflush=%lums maxhealth=%lums sd=%s health=%lu run=%u | "
                     "nmea=%lu badcrc=%lu fix=%s | heap=%lu heapmin=%lu",
                     (unsigned long)radioPacketCount(), (unsigned long)radioCrcErrorCount(),
                     (unsigned long)radioQueueDropCount(), (unsigned long)radioBusMissCount(),
                     (unsigned long)loggerRowsWritten(), (unsigned long)loggerRowsDropped(),
                     (unsigned long)loggerIdentityRowsWritten(),
                     (unsigned long)(radioIdentityDropCount() + loggerIdentityRowsDropped()),
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
        if (lock.held()) serialPrintln(line);
    }
}
