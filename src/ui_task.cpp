#include "ui_task.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "backlight.h"
#include "battery.h"
#include "board_pins.h"
#include "detection.h"
#include "display_settings.h"
#include "gps_task.h"
#include "keyboard.h"
#include "logger_task.h"
#include "radio_task.h"
#include "spi_bus.h"
#include "ui_labels.h"
#include "ui_menu.h"
#include "version.h"
#include "wifi_task.h"

namespace {

// tft is the draw target every function below writes to. As of the Phase 6
// bench pass (2026-08-25) it points at an off-screen Arduino_Canvas_Indexed
// buffer, not the physical panel directly — see uiTaskStart() for why.
Arduino_GFX *tft = nullptr;
Arduino_Canvas_Indexed *canvas = nullptr;

// TCA8418 keyboard controller. Cardputer ADV replaced the base Cardputer's
// GPIO matrix with this I2C part — same SDA/SCL as the IO expander, a
// different address, which is ordinary shared-bus operation.
//
// CRITICAL: the TCA8418 boots in SLEEP and reports nothing until explicitly
// configured, even when I2C is perfectly healthy. Exactly the failure shape
// as the GPS power rail (a working bus proving nothing about a working
// device). begin() + matrix() is the wake sequence, taken from
// bmorcelli/Launcher's confirmed-working Cardputer-ADV interface.
Adafruit_TCA8418 keys;
bool keyboardReady = false;

UiPage page = UiPage::RADIO;
uint32_t lastPageChange = 0;

// Phase 6 root table, revised four times 2026-08-25 (BRAND.md's "Revised
// again" note has the Profile/System history). "Profile" opens onto the
// real, technical profile names — Meshtastic, MeshCore today; Reticulum/
// Spectrum join once they have a real HOME_LISTEN table (Phase 8) — instead
// of cycling one at a time on Enter the way Phase 4/5 did. Deliberately not
// branded as "Mesh Trace" or similar: these are LoRa presets on one
// sniffer, not sibling products, and BRAND.md already had "Profile" as its
// preferred word for exactly this axis before an earlier same-day revision
// briefly tried a per-profile brand name instead. "Trace" is the one
// ACTION root row — a live pause/standby toggle for the radio-listening +
// logging pipeline, promoted out of System the same session it shipped on
// operator feedback that it's central enough to fire without drilling into
// a group first; see its own comment below for why it isn't called
// "MeshTrace". Three root rows is not a hardcoded ceiling — DISCOVERY_SWEEP
// (Phase 7) / ENERGY_SWEEP (Phase 8) each get their own row or group here
// without touching MenuState itself, which is the whole point of this
// redesign (ROADMAP.md Phase 6, PROGRESS.md 2026-08-25 Decisions log).
constexpr MenuItem PROFILE_GROUP_ITEMS[] = {
    {"Meshtastic", ItemKind::ACTION, MenuAction::SELECT_MESHTASTIC, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"MeshCore", ItemKind::ACTION, MenuAction::SELECT_MESHCORE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
// Display (2026-08-25, third revision): Brightness/idle-dim moved out of
// System's own flat list into their own nested group on operator request
// ("system > display > display relevant settings") — the first thing in
// this project to actually need ui_menu.h's generalized nesting (System's
// own WiFi/Debug/Trace grouping was flat, one level, from the start; this
// is a GROUP inside a GROUP). Brightness stays a SLIDER row, just reached
// one level deeper than when it briefly lived at root.
constexpr MenuItem DISPLAY_GROUP_ITEMS[] = {
    {"Brightness", ItemKind::SLIDER, MenuAction::NONE, MenuAction::BRIGHTNESS_UP, MenuAction::BRIGHTNESS_DOWN, nullptr, 0},
    // Idle-dim timeout: cycles Off/30s/60s/2min/5min on each Enter press,
    // same "fires and stays in the list" shape WiFi/Debug already have,
    // just cycling a value instead of flipping a bool — an operator asked
    // for real choices here instead of a hardcoded 60s.
    {"Idle dim", ItemKind::ACTION, MenuAction::IDLE_TIMEOUT_CYCLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
constexpr MenuItem SYSTEM_GROUP_ITEMS[] = {
    {"WiFi", ItemKind::ACTION, MenuAction::WIFI_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Debug", ItemKind::ACTION, MenuAction::DEBUG_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Display", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, DISPLAY_GROUP_ITEMS, 2},
};
// Trace pause/standby (2026-08-25) is a root-level ACTION row, not a System
// item — promoted the same day it shipped, on operator feedback that it's
// central enough to toggle without drilling into System first. Deliberately
// named "Trace" alone, not "MeshTrace": that exact compound word was walked
// back earlier this same session (BRAND.md's Interface Naming table) for
// overloading "Trace" across the product name, a per-profile brand, and a
// saved-session noun — reviving it here for a fourth meaning (live radio
// state) would repeat the same mistake. Puts the SX1262 in its warm sleep
// mode instead of continuous RX — a real battery lever, unlike GPS
// (io_expander.h: GPS power shares the antenna-switch line, so there's no
// independent GPS power to save, and this deliberately leaves GPS running
// so position is already fresh the instant Trace resumes).
constexpr MenuItem ROOT_ITEMS[] = {
    {"Trace", ItemKind::ACTION, MenuAction::TRACE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Profile", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, PROFILE_GROUP_ITEMS, 2},
    {"System", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 3},
};
constexpr uint8_t ROOT_COUNT = 3;
MenuState menu(ROOT_ITEMS, ROOT_COUNT);

// Toast layer (Phase 6): a brief overlay message for feedback that isn't
// tied to whichever menu row happens to be highlighted — e.g. confirming a
// toggle fired right before BACK leaves the menu. Only this static buffer,
// not a dynamic allocation — no heap number to gate behind the way WiFi's
// AP needed one. (The earlier "no canvas/framebuffer, direct-to-panel"
// note that used to sit here no longer holds: the bench pass that put the
// panel in front of this code for the first time found real flicker/tear
// direct-to-panel drawing can't avoid, and uiTaskStart() now draws through
// an off-screen Arduino_Canvas_Indexed instead — see its comment there.)
char toastMsg[48] = {0};
uint32_t toastShownAt = 0;
constexpr uint32_t TOAST_DURATION_MS = 1400;
constexpr uint32_t TOAST_SLIDE_MS = 150;
constexpr int16_t TOAST_H = 16;

void showToast(const char *msg) {
    strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
    toastMsg[sizeof(toastMsg) - 1] = '\0';
    toastShownAt = millis();
}

bool toastActive() {
    return toastMsg[0] != '\0' && (millis() - toastShownAt) < TOAST_DURATION_MS;
}

// RX activity pulse (Phase 6 UI redesign): a brief, event-driven flash on
// the header's third status dot and a matching flash line on RADIO,
// replacing the old idle heartbeat blink — that only ever proved the UI
// task's own loop was alive, not that anything was actually being heard.
// Binary hold-then-revert, not the alpha-blended decay the design mockup
// used: Arduino_GFX/RGB565 has no cheap alpha blending, so "recently
// active" is a solid color held for RX_PULSE_MS and then reverted, rather
// than a fading glow.
uint32_t rxPulseUntil = 0;
constexpr uint32_t RX_PULSE_MS = 220;

bool rxPulseActive() {
    return millis() < rxPulseUntil;
}

// Brightness + idle-dim (2026-08-25, second revision — slider + persisted
// settings). activeBrightnessPercent is the operator's chosen level (5-100,
// BRIGHTNESS_STEP at a time) — what idle-dim restores to on the next
// keypress, not necessarily what the backlight is driven at right now
// (which may be a lower idle floor while displayDimmed — see
// idleDimTargetPercent() below). Seeded from SD (uiTaskStart()'s
// `settings` param) instead of a hardcoded default, so the device
// remembers what an operator picked across power cycles, same as channel
// overrides already do.
uint8_t activeBrightnessPercent = 100;
constexpr uint8_t BRIGHTNESS_MIN = 5;
constexpr uint8_t BRIGHTNESS_MAX = 100;
constexpr uint8_t BRIGHTNESS_STEP = 5;
bool displayDimmed = false;

// Idle-dim timeout, cycled from System's "Idle dim" group item
// (IDLE_TIMEOUT_CYCLE) instead of a plain on/off — index 0 is "Off"
// (disables idle-dim entirely, matching what the old AUTODIM_TOGGLE=false
// used to mean), the rest are real durations. Index 2 (60s) is the
// default, matching this feature's original hardcoded value. Also seeded
// from SD.
struct IdleTimeoutOption {
    const char *label;
    uint32_t ms; // unused when index 0 ("Off")
};
constexpr IdleTimeoutOption IDLE_TIMEOUT_OPTIONS[] = {
    {"Off", 0},
    {"30s", 30000},
    {"60s", 60000},
    {"2min", 120000},
    {"5min", 300000},
};
constexpr uint8_t IDLE_TIMEOUT_OPTION_COUNT = 5;
uint8_t idleTimeoutIndex = 2;

// lastKeyActivity tracks any recognized KeyAction, the same basis
// AUTO_ADVANCE_MS's carousel-timer already uses for "idle" — on a
// keyboardless unit (!keyboardReady) this never advances past boot, so the
// display dims at the configured timeout and stays dimmed for the rest of
// an unattended run. That's the right outcome for exactly the multi-hour-
// unattended-drive scenario this project is built around, not a corner
// case to special-case away.
uint32_t lastKeyActivity = 0;

// The level idle-dim actually drives when it engages: the lower of a fixed
// floor and the operator's own active level. Needed once brightness became
// a slider that can go below the old fixed IDLE_DIM_PERCENT (15) — without
// this, setting an active level of e.g. 10% and then going idle would make
// the screen get BRIGHTER (jump to 15%) instead of dimmer.
constexpr uint8_t IDLE_DIM_FLOOR = 15;
uint8_t idleDimTargetPercent() {
    return activeBrightnessPercent < IDLE_DIM_FLOOR ? activeBrightnessPercent : IDLE_DIM_FLOOR;
}

// Without a keyboard the pages rotate on their own — a device stuck on one
// page during a multi-hour field test is worse than one that cycles.
constexpr uint32_t AUTO_ADVANCE_MS = 8000;
// Idle redraw cadence (battery/heartbeat-equivalent staleness guard).
constexpr uint32_t REDRAW_MS = 1000;
// Redraw cadence while the toast slide/countdown or the RX pulse is
// actively animating — a bounded burst, not a continuous animation loop:
// it only runs for TOAST_DURATION_MS after a toast fires, or RX_PULSE_MS
// after a detection, then falls back to REDRAW_MS.
constexpr uint32_t FAST_REDRAW_MS = 60;

// 240x135 at rotation 1. Text size 1 is 6x8px; size 2 is 12x16px.
constexpr int16_t HEADER_H = 12;
constexpr uint16_t COL_BG = 0x0000;     // black
constexpr uint16_t COL_FG = 0xFFFF;     // white
constexpr uint16_t COL_DIM = 0x8410;    // grey
constexpr uint16_t COL_GOOD = 0x07E0;   // green
constexpr uint16_t COL_WARN = 0xFFE0;   // yellow
constexpr uint16_t COL_BAD = 0xF800;    // red

const char *pageName(UiPage p) {
    switch (p) {
        case UiPage::RADIO: return "RADIO";
        case UiPage::CHANNEL: return "CHANNEL";
        case UiPage::GPS: return "GPS";
        case UiPage::SYSTEM: return "SYSTEM";
        default: return "?";
    }
}

// Battery glyph + percentage, top right. Drawn on every page so the number
// an operator most wants mid-field is never more than a glance away.
void drawBattery() {
    const uint32_t mv = batteryMilliVolts();
    const int16_t w = 22, h = 9;
    const int16_t x = tft->width() - w - 4;
    const int16_t y = 2;

    tft->fillRect(x - 30, y - 1, w + 34, h + 2, COL_BG);

    if (mv == 0) {
        // Unknown, not empty. Drawing 0% would imply a dying battery when
        // the truth is the ADC gave an implausible reading (USB-only, no
        // cell fitted, etc.).
        tft->setTextSize(1);
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(x - 28, y);
        tft->print("bat ?");
        return;
    }

    const uint8_t pct = batteryPercentFromMv(mv);
    const uint16_t colour = (pct >= 50) ? COL_GOOD : (pct >= 20 ? COL_WARN : COL_BAD);

    tft->setTextSize(1);
    tft->setTextColor(colour, COL_BG);
    tft->setCursor(x - 30, y);
    tft->print(pct);
    tft->print('%');

    tft->drawRect(x, y, w, h, colour);
    tft->fillRect(x + w, y + 2, 2, h - 4, colour); // terminal nub
    const int16_t fill = (int16_t)((w - 2) * pct / 100);
    if (fill > 0) tft->fillRect(x + 1, y + 1, fill, h - 2, colour);
}

// GPS-fix header dot. Reads with a zero-tick (non-blocking) mutex try and
// keeps the last known color when the mutex is busy, rather than blocking
// — drawHeader() runs at FAST_REDRAW_MS during an active toast/pulse, and
// a blocking wait at that cadence would add needless mutex pressure for a
// dot that only needs to be roughly current, not per-frame exact.
uint16_t gpsStatusColour() {
    static uint16_t cached = COL_DIM;
    GpsFix fix;
    if (gpsGetFix(fix, 0)) {
        cached = fix.has_position ? COL_GOOD : (fix.sats_in_view > 0 ? COL_WARN : COL_BAD);
    }
    return cached;
}

// Shared 3-tier colour for every heap-usage display (header dot, SYSTEM's
// "k heap" text, and its bar below) — green under 80% of the ~512KB
// no-PSRAM SRAM budget (DESIGN.md §1) used, yellow 80-90%, red above 90%.
// Takes free heap in KB so SYSTEM's page (which already computes that for
// its own text) doesn't redo the division. Replaces the old flat
// "<100000 bytes free" single threshold — close to the same cutover point
// (~102KB free is 80% used of 512KB) but with the escalating red tier this
// bar's redesign asked for, rather than staying stuck on yellow all the way
// to empty.
constexpr uint32_t HEAP_BUDGET_KB = 512;
uint16_t heapUsageColour(uint32_t freeHeapK) {
    const uint32_t usedK = (freeHeapK < HEAP_BUDGET_KB) ? (HEAP_BUDGET_KB - freeHeapK) : 0;
    const float usedFrac = (float)usedK / (float)HEAP_BUDGET_KB;
    if (usedFrac >= 0.90f) return COL_BAD;
    if (usedFrac >= 0.80f) return COL_WARN;
    return COL_GOOD;
}

// Heap-health header dot — same thresholds drawSystemPage() colors "k heap"
// and its bar by, just always visible instead of only on its own page.
uint16_t heapStatusColour() {
    return heapUsageColour(ESP.getFreeHeap() / 1024);
}

void drawHeader() {
    tft->fillRect(0, 0, tft->width(), HEADER_H, COL_BG);
    tft->setTextSize(1);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, 2);

    if (menu.isOpen()) {
        tft->print("MENU");
        // Breadcrumb, e.g. "MENU > System > Display" — tells an operator
        // how deep they are without needing to back out to check. Full
        // ancestor chain now that nesting is real (2026-08-25) — not just
        // one group name, since a GROUP can itself open another GROUP.
        // "MENU > System > Display > Brightness" (worst case today, deepest
        // path) is 36 chars at size-1 text (6px/char = 216px), clear of
        // the 240px edge.
        tft->setTextColor(COL_DIM, COL_BG);
        for (uint8_t i = 0; i < menu.breadcrumbCount(); i++) {
            tft->print(" > ");
            tft->print(menu.breadcrumbLabel(i));
        }
        if (menu.inSlider()) {
            tft->print(" > ");
            tft->print(menu.currentItem().label);
        }
    } else {
        // Page name only — profile and page position moved to the footer
        // status line (drawFooterStatus() below) so this text never
        // crowds the status-dot cluster or the battery reading on either
        // side of it (bench feedback, PROGRESS.md 2026-08-25 Decisions
        // log).
        tft->print(pageName(page));
    }

    drawBattery();

    // Status dot cluster (Phase 6): GPS fix state, heap health, and RX
    // activity, always visible from any page instead of only their own
    // dedicated one. 5px clear of the battery reading (bench feedback) —
    // replaces the old idle heartbeat blink entirely.
    tft->fillCircle(157, 6, 2, gpsStatusColour());
    tft->fillCircle(166, 6, 2, heapStatusColour());
    tft->fillCircle(175, 6, 2, rxPulseActive() ? COL_GOOD : COL_DIM);

    tft->drawFastHLine(0, HEADER_H, tft->width(), COL_DIM);
}

// Persistent status line in the footer band (Phase 6) — profile
// left-anchored, page position right-anchored. Carousel only: the menu
// already shows the active profile on its own "Profile" root row, and
// the header breadcrumb already says which group is open. Drawn before
// drawToast() so a toast simply paints over it for its duration and this
// reappears once the toast clears.
void drawFooterStatus() {
    if (menu.isOpen()) return;
    const int16_t y = tft->height() - 10;
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);

    tft->setCursor(2, y);
    tft->print(uiProfileLabel(radioActiveProfile()));

    char posBuf[8];
    snprintf(posBuf, sizeof(posBuf), "%u/%u", (unsigned)page + 1, (unsigned)UiPage::COUNT);
    tft->setCursor(tft->width() - (int16_t)strlen(posBuf) * 6 - 2, y);
    tft->print(posBuf);
}

// A small "label above value" block, used for the secondary/context column
// every redesigned page below carries alongside its primary left-column
// numbers — Phase 6's answer to the old single-column layout's unused right
// half (PROGRESS.md 2026-08-25 Decisions log). One shared helper instead of
// each page inventing its own right-column formatting, since every page
// now does this.
void statBlock(int16_t x, int16_t y, const char *label, const char *value, uint16_t valueColour = COL_FG) {
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(x, y);
    tft->print(label);
    tft->setTextColor(valueColour, COL_BG);
    tft->setCursor(x, y + 9);
    tft->print(value);
}

void drawRadioPage() {
    const uint32_t drops = radioQueueDropCount() + loggerRowsDropped();
    char buf[16];

    // Left column — the three numbers an operator checks first.
    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print("rx ");
    tft->print(radioPacketCount());

    tft->setCursor(2, HEADER_H + 26);
    tft->print("log ");
    tft->print(loggerRowsWritten());
    // RX activity pulse — solid flash under "log" while a detection was
    // heard in the last RX_PULSE_MS; reverts on its own because drawPage()
    // clears this whole region before every redraw.
    if (rxPulseActive()) {
        tft->fillRect(2, HEADER_H + 23, 58, 2, COL_GOOD);
    }

    // Drops are the number that decides whether the architecture is holding
    // up under real traffic, so they get colour rather than being buried.
    tft->setTextColor(drops == 0 ? COL_GOOD : COL_BAD, COL_BG);
    tft->setCursor(2, HEADER_H + 46);
    tft->print("drop ");
    tft->print(drops);

    // STANDBY banner (Trace pause/standby, System menu): rx/log/drop above
    // stay as real frozen totals rather than being replaced — the thing
    // that must not be ambiguous is "is the radio actually listening right
    // now," not the counters themselves, which are still meaningful while
    // paused. Sits in the gap between the hero column and the bottom flush
    // band, which RADIO's layout otherwise leaves empty (unlike GPS/SYSTEM,
    // which use it for their own secondary content).
    if (radioIsTracePaused()) {
        tft->setTextSize(2);
        tft->setTextColor(COL_WARN, COL_BG);
        tft->setCursor(2, HEADER_H + 66);
        tft->print("STANDBY");
    }

    // Right column — x=170, the same start GPS's sats/qual column uses, so
    // the right column lands in the same physical 70px-wide zone on every
    // page instead of drifting per page (bench feedback, PROGRESS.md
    // 2026-08-25 Decisions log).
    constexpr int16_t RIGHT_X = 170;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)radioCrcErrorCount());
    statBlock(RIGHT_X, HEADER_H + 6, "crc", buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)radioBusMissCount());
    statBlock(RIGHT_X, HEADER_H + 28, "miss", buf);
    statBlock(RIGHT_X, HEADER_H + 50, "sd", loggerSdReady() ? "ok" : "DOWN",
              loggerSdReady() ? COL_FG : COL_BAD);
    snprintf(buf, sizeof(buf), "r%u", (unsigned)loggerRunIndex());
    statBlock(RIGHT_X, HEADER_H + 72, "run", buf);

    // Bottom band, full width — flush stats matter for judging whether
    // BATCH_BUF_SIZE needs retuning (DESIGN.md §8.2), not for a quick
    // glance, so they sit below both columns rather than competing with
    // either for attention.
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 94);
    tft->print("flush ");
    tft->print(loggerFlushCount());
    tft->print("  max ");
    tft->print(loggerMaxFlushMs());
    tft->print("ms");
}

// Track + marker, not a fill bar — frequency is a *position* within the
// module's tuned range, not a proportion of something used up (unlike
// drawHeapBar() below). 868-923MHz is the SX1262 front end's actual tuned
// range (DESIGN.md §1), not the full 902-928MHz US ISM band — the point is
// showing real margin to that edge, not just an arbitrary axis.
void drawFreqBar(int16_t x, int16_t y, int16_t w, float freqMhz) {
    constexpr float LO = 868.0f, HI = 923.0f;
    tft->drawFastHLine(x, y, w, COL_DIM);
    float frac = (freqMhz - LO) / (HI - LO);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t mx = x + (int16_t)((w - 3) * frac);
    tft->fillRect(mx, y - 3, 3, 7, COL_GOOD);
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(x, y + 6);
    tft->print((int)LO);
    char hiBuf[8];
    snprintf(hiBuf, sizeof(hiBuf), "%d", (int)HI);
    tft->setCursor(x + w - (int16_t)strlen(hiBuf) * 6, y + 6);
    tft->print(hiBuf);
}

// Rough estimate, deliberately not the full LoRa airtime spec (skips
// explicit header/CRC/low-data-rate-optimization bits) — enough to compare
// configs at a glance ("SF11 costs ~4x SF7 here"), not to cite as an exact
// figure. ~36 symbols approximates a typical Meshtastic/MeshCore packet's
// preamble + header + payload.
uint32_t estimateTimeOnAirMs(uint8_t sf, float bwKhz) {
    const float symbolMs = (float)(1u << sf) / (bwKhz * 1000.0f) * 1000.0f;
    return (uint32_t)(symbolMs * 36.0f + 0.5f);
}

// Read-only RF detail behind RADIO's counters — added Phase 5 alongside the
// menu's live profile switch, so an operator has an on-device way to
// confirm a switch actually retuned the radio (radioActiveChannel() already
// reflects it correctly post-switch, Phase 4) rather than trusting the
// header text alone. Spread down the full page height instead of clustered
// under the header (Phase 6): the previous layout packed freq/SF-BW/bar/
// CR-sync into the top ~70px and left the bottom third empty, which read as
// top-heavy and cluttered on top of that (PROGRESS.md 2026-08-25 Decisions
// log).
void drawChannelPage() {
    const ChannelParams ch = radioActiveChannel();

    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print(ch.freq_mhz, 3);
    tft->print(" MHz");

    tft->setCursor(2, HEADER_H + 32);
    tft->print("SF");
    tft->print(ch.sf);
    tft->print(" BW");
    tft->print(ch.bw_khz, 1);

    drawFreqBar(2, HEADER_H + 62, 108, ch.freq_mhz);

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 94);
    tft->print("CR4/");
    tft->print(ch.cr_denom);
    tft->print("  sync 0x");
    tft->print(ch.sync_word, HEX);

    // Right column — x=170, same start RADIO/GPS use. The radio-mode label
    // (BRAND.md's "Watch" for HOME_LISTEN) is the only one of the three
    // mode labels with anything to name until Phase 7 (Probe) / Phase 8
    // (Sweep) add the other two radio states this same slot will grow
    // into.
    statBlock(170, HEADER_H + 6, "mode", uiModeLabelWatch());
    char airtimeBuf[16];
    snprintf(airtimeBuf, sizeof(airtimeBuf), "~%lums", (unsigned long)estimateTimeOnAirMs(ch.sf, ch.bw_khz));
    statBlock(170, HEADER_H + 34, "airtime", airtimeBuf);
}

// 4 small bars instead of a dim "GP:12 GL:6 GA:2 BD:2" text line — same
// per-constellation counts (fix.talkers), scannable at a glance rather than
// read digit by digit. Runs in both fix states, same as the text line it
// replaces (GSV sentences report in-view sats before a fix exists too).
// Capped at 4 talkers: the right column is 70px wide (x=170..240) and 4
// bars at a 16px pitch fit it with room to spare.
void drawConstellationBars(int16_t x, int16_t y, int16_t h, const GpsFix &fix) {
    if (fix.talker_count == 0) return;
    uint16_t maxCount = 1;
    for (uint8_t i = 0; i < fix.talker_count && i < 4; i++) {
        if (fix.talkers[i].in_view > maxCount) maxCount = fix.talkers[i].in_view;
    }
    constexpr int16_t BAR_W = 12, PITCH = 16;
    for (uint8_t i = 0; i < fix.talker_count && i < 4; i++) {
        const int16_t bx = x + i * PITCH;
        int16_t barH = (int16_t)((uint32_t)h * fix.talkers[i].in_view / maxCount);
        if (barH < 1) barH = 1;
        tft->fillRect(bx, y + h - barH, BAR_W, barH, COL_GOOD);
        tft->setTextSize(1);
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(bx, y + h + 3);
        tft->print(fix.talkers[i].id);
    }
}

void drawGpsPage() {
    GpsFix fix;
    const bool have = gpsGetFix(fix, pdMS_TO_TICKS(100));

    tft->setTextSize(2);
    if (have && fix.has_position) {
        tft->setTextColor(COL_GOOD, COL_BG);
        tft->setCursor(2, HEADER_H + 6);
        tft->print(fix.fix_type >= 3 ? "3D FIX" : "2D FIX");

        tft->setTextColor(COL_FG, COL_BG);
        tft->setCursor(2, HEADER_H + 28);
        tft->print(fix.lat, 5);
        tft->setCursor(2, HEADER_H + 48);
        tft->print(fix.lon, 5);

        // Right column — satellites USED, a real gap the old layout had:
        // it showed sats-in-view before a fix (the leading indicator) but
        // never showed the used count once a fix actually landed.
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)fix.satellites);
        statBlock(170, HEADER_H + 6, "sats", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)fix.fix_quality);
        statBlock(170, HEADER_H + 28, "qual", buf);
    } else {
        // Before a fix, sats-IN-VIEW is the number that matters: it says
        // whether the antenna can see sky at all, minutes before a fix
        // lands. `satellites` (used) stays 0 until then and would look
        // identical whether the antenna were working or disconnected.
        tft->setTextColor(fix.sats_in_view > 0 ? COL_WARN : COL_BAD, COL_BG);
        tft->setCursor(2, HEADER_H + 6);
        tft->print("NO FIX");

        tft->setTextColor(COL_FG, COL_BG);
        tft->setCursor(2, HEADER_H + 30);
        tft->print("view ");
        tft->print(fix.sats_in_view);

        tft->setTextSize(1);
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(2, HEADER_H + 52);
        tft->print(fix.sats_in_view > 0 ? "acquiring, keep still" : "no sky - go outside");
    }

    // Right column, under sats/qual (or under NO FIX in the no-fix branch,
    // which otherwise left that whole side empty) — moved here from the
    // old dim text line in the left column's open width, and other pages
    // now share this same 170..240 zone (bench feedback, PROGRESS.md
    // 2026-08-25 Decisions log).
    drawConstellationBars(170, HEADER_H + 58, 14, fix);

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 72);
    if (have && fix.has_time) {
        char ts[24];
        detectionFormatTimestamp(ts, sizeof(ts), true, fix.year, fix.month, fix.day, fix.hour,
                                 fix.minute, fix.second);
        tft->print(ts);
    } else {
        tft->print("nmea ");
        tft->print(gpsSentenceCount());
        tft->print(" crc ");
        tft->print(gpsChecksumErrorCount());
    }
}

// Outline + proportional fill, same visual language as drawBattery() above
// — turns "312k heap" into something scannable at a glance instead of a
// number to read and compare against 512 in your head. ~512KB is the
// ESP32-S3FN8's total SRAM with no PSRAM (DESIGN.md §1) — an upper bound
// for context, not a claim about free-heap-at-boot.
//
// Fills with USAGE, not remaining free space (flipped 2026-08-25, bench
// feedback): a bar that grows as the budget gets consumed reads the same
// direction as the colour tiers above it (both escalate toward "full is
// bad"), where the old free-space fill emptied toward the danger zone —
// visually backwards next to a bar whose colour was getting more alarming
// as it got shorter.
void drawHeapBar(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t freeHeapK, uint16_t colour) {
    tft->drawRect(x, y, w, h, colour);
    const uint32_t usedK = (freeHeapK < HEAP_BUDGET_KB) ? (HEAP_BUDGET_KB - freeHeapK) : HEAP_BUDGET_KB;
    float frac = (float)usedK / (float)HEAP_BUDGET_KB;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t fill = (int16_t)((w - 2) * frac);
    if (fill > 0) tft->fillRect(x + 1, y + 1, fill, h - 2, colour);
}

// Merged with the old WIFI carousel page (Phase 6 UI redesign, 2026-08-25):
// AP on/off plus client count didn't need a whole carousel slot once SYSTEM
// had room for a 2x2 grid. SSID moved into the toast message WIFI_TOGGLE
// fires (fireMenuAction() below) instead of living on this page, which now
// shows only ON/OFF + client count. Reworked once already the same day
// after the merge first shipped: the stat column sat far right of the hero
// numbers with a wide empty gap, and dead space remained below everything.
// Now a 2x2 grid pulled in closer to the hero column, and a heap bar fills
// what used to be blank space under "k heap" with an actual gauge instead
// of just relocating the same lines of text.
void drawSystemPage() {
    char buf[16];

    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print(millis() / 60000);
    tft->print(" min");

    const uint32_t heap = ESP.getFreeHeap();
    const uint16_t heapColour = heapUsageColour(heap / 1024);
    tft->setTextColor(heapColour, COL_BG);
    tft->setCursor(2, HEADER_H + 28);
    tft->print(heap / 1024);
    tft->print("k heap");
    drawHeapBar(2, HEADER_H + 48, 108, 8, heap / 1024, heapColour);

    // Col A: x=136, clearing the heap bar's right edge (x=110) by 26px.
    // Col B: x=205 — nudged to hold its own gap from col A rather than
    // both columns crowding together or col B running off the panel
    // (bench feedback, PROGRESS.md 2026-08-25 Decisions log — "205+30"
    // for the longest value, "3.98V", is about as far right as that still
    // fits with any margin).
    snprintf(buf, sizeof(buf), "%luk", (unsigned long)(ESP.getMinFreeHeap() / 1024));
    statBlock(136, HEADER_H + 6, "min heap", buf);

    const uint32_t mv = batteryMilliVolts();
    if (mv == 0) {
        statBlock(205, HEADER_H + 6, "batt", "unknown");
    } else {
        snprintf(buf, sizeof(buf), "%.2fV", mv / 1000.0);
        statBlock(205, HEADER_H + 6, "batt", buf);
    }

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)spiBusContentionCount());
    statBlock(136, HEADER_H + 36, "bus", buf);

    const bool wifiOn = wifiIsEnabled();
    if (wifiOn) {
        snprintf(buf, sizeof(buf), "ON %u", (unsigned)wifiClientCount());
    } else {
        snprintf(buf, sizeof(buf), "OFF");
    }
    statBlock(205, HEADER_H + 36, "wifi", buf, wifiOn ? COL_GOOD : COL_DIM);

    // Bottom band, full width — lower-priority context: keyboard presence,
    // health-row count (confirms the session log is actually being written
    // before driving off with the lid shut), firmware version.
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 66);
    tft->print("keys ");
    tft->print(keyboardReady ? "tca8418" : "none (auto)");
    tft->print("  health ");
    tft->print(loggerSessionRows());

    tft->setCursor(2, HEADER_H + 78);
    tft->print(FIRMWARE_VERSION);
}

// One row of a menu list (root or group), selected or not. Selection is
// carried by inverting FG/BG on that row's whole width — same
// colour-carries-state convention drawBattery()/drawRadioPage() already
// use, just applied to a full row instead of a single value. The label/
// value separator lives here, not baked into each label string as a
// trailing space (Phase 5's convention) — a table entry forgetting the
// space is an easy, silent mistake this removes entirely.
void drawMenuRow(int16_t y, const char *rowLabel, const char *value, bool selected) {
    const uint16_t fg = selected ? COL_BG : COL_FG;
    const uint16_t bg = selected ? COL_FG : COL_BG;
    tft->fillRect(0, y - 3, tft->width(), 20, bg);
    tft->setTextSize(2);
    tft->setTextColor(fg, bg);
    tft->setCursor(4, y);
    tft->print(rowLabel);
    if (value != nullptr && value[0] != '\0') {
        tft->print(' ');
        tft->print(value);
    }
}

// What an ACTION row's value column shows. Generic over every list in the
// menu (Profile's choices, System's toggles, Display's Idle-dim cycle) —
// MenuState/MenuItem are data-driven on purpose (ui_menu.h), and this stays
// a single switch on MenuAction rather than one function per list, the
// same way drawMenuList() below stays one function for every depth.
const char *menuEntryValue(MenuAction action) {
    switch (action) {
        case MenuAction::SELECT_MESHTASTIC:
            return radioActiveProfile() == MissionProfile::MESHTASTIC ? "ACTIVE" : "";
        case MenuAction::SELECT_MESHCORE:
            return radioActiveProfile() == MissionProfile::MESHCORE ? "ACTIVE" : "";
        case MenuAction::WIFI_TOGGLE: return wifiIsEnabled() ? "ON" : "OFF";
        case MenuAction::DEBUG_TOGGLE: return loggerDebugIsEnabled() ? "ON" : "OFF";
        case MenuAction::IDLE_TIMEOUT_CYCLE: return IDLE_TIMEOUT_OPTIONS[idleTimeoutIndex].label;
        // TRACE_TOGGLE has no case here: it's a root-level ACTION row whose
        // live state is rendered directly below rather than through this
        // ACTION-only lookup. BRIGHTNESS_UP/DOWN aren't ACTION rows at all
        // (SLIDER kind) so never reach this function.
        default: return "";
    }
}

// One list of rows, at whatever depth menu.currentList() currently is —
// the root list, System's list, or Display's nested list all draw through
// this same function now that nesting is real (2026-08-25); no more
// separate drawMenuRoot()/drawMenuGroup() special-cased by depth.
void drawMenuList() {
    const MenuItem *list = menu.currentList();
    const uint8_t count = menu.currentCount();
    for (uint8_t i = 0; i < count; i++) {
        const MenuItem &item = list[i];
        char label[24];
        if (item.action == MenuAction::TRACE_TOGGLE) {
            // "Trace: Active"/"Trace: Standby" — colon format, consistent
            // with Profile/Brightness's own live-value rows below. State
            // words match RADIO page's STANDBY banner and
            // drawBattery()-style ALL-CAPS convention elsewhere in this
            // file.
            snprintf(label, sizeof(label), "Trace: %s", radioIsTracePaused() ? "STANDBY" : "ACTIVE");
            drawMenuRow(HEADER_H + 10 + i * 24, label, nullptr, menu.currentIndex() == i);
        } else if (item.kind == ItemKind::SLIDER) {
            // Brightness is the only SLIDER row in the app today, so this
            // reaches straight for activeBrightnessPercent rather than
            // being fully generic over "whichever slider" — worth
            // revisiting if a second slider ever gets added. "Brightness:
            // 100%" = 17 chars = 204px at size-2 text, clear of the 240px
            // edge that bit the old Profile row before "> " was dropped.
            snprintf(label, sizeof(label), "Brightness: %u%%", (unsigned)activeBrightnessPercent);
            drawMenuRow(HEADER_H + 10 + i * 24, label, nullptr, menu.currentIndex() == i);
        } else if (item.items == PROFILE_GROUP_ITEMS) {
            // "Profile: Meshtastic" — the row still surfaces the live
            // profile so the active protocol reads without drilling into
            // the group. This format tops out at "Profile: Meshtastic"
            // (19 chars = 228px), clear of the panel's 240px edge.
            snprintf(label, sizeof(label), "Profile: %s", uiProfileLabel(radioActiveProfile()));
            drawMenuRow(HEADER_H + 10 + i * 24, label, nullptr, menu.currentIndex() == i);
        } else if (item.kind == ItemKind::GROUP) {
            // Plain label, no value — matches System's/Display's existing
            // bare-list-row look ("a bare list is already legibly a menu",
            // bench feedback 2026-08-25).
            drawMenuRow(HEADER_H + 10 + i * 24, item.label, nullptr, menu.currentIndex() == i);
        } else {
            drawMenuRow(HEADER_H + 10 + i * 24, item.label, menuEntryValue(item.action), menu.currentIndex() == i);
        }
    }

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. move   Enter select   ` back");
}

// Brightness slider screen (2026-08-25) — the one SLIDER view today. Large
// live readout plus a filled-bar track, same outline+fill visual language
// drawHeapBar()/drawFreqBar() already use elsewhere in this file rather
// than inventing a third bar style.
void drawMenuSlider() {
    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 10);
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)activeBrightnessPercent);
    tft->print(pctBuf);

    constexpr int16_t BAR_X = 2, BAR_Y = HEADER_H + 40, BAR_W = 200, BAR_H = 14;
    tft->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_GOOD);
    const float frac = (float)(activeBrightnessPercent - BRIGHTNESS_MIN) / (float)(BRIGHTNESS_MAX - BRIGHTNESS_MIN);
    const int16_t fill = (int16_t)((BAR_W - 2) * frac);
    if (fill > 0) tft->fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, COL_GOOD);

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. adjust +/-5%   ` back");
}

// Toast overlay (Phase 6) — flush-bottom band that slides up from
// off-panel on show and carries a shrinking countdown bar along its own
// bottom edge. Both are just per-frame rectangle geometry (no alpha
// blending needed, unlike the design mockup's decaying RX-pulse glow),
// driven by the bounded fast-redraw burst in uiTask() below while
// toastActive(), not a continuous animation loop. Drawn last, on top of
// whatever page/menu is showing.
void drawToast() {
    if (!toastActive()) return;
    const uint32_t elapsed = millis() - toastShownAt;
    const float slideT = elapsed >= TOAST_SLIDE_MS ? 1.0f : (float)elapsed / (float)TOAST_SLIDE_MS;
    const int16_t y = tft->height() - (int16_t)(TOAST_H * slideT);

    tft->fillRect(0, y, tft->width(), TOAST_H, COL_FG);
    tft->setTextSize(1);
    tft->setTextColor(COL_BG, COL_FG);
    tft->setCursor(4, y + 4);
    tft->print(toastMsg);

    float remain = 1.0f - (float)elapsed / (float)TOAST_DURATION_MS;
    if (remain < 0.0f) remain = 0.0f;
    const int16_t barW = (int16_t)(tft->width() * remain);
    if (barW > 0) tft->fillRect(0, y + TOAST_H - 2, barW, 2, COL_DIM);
}

void drawPage() {
    tft->fillRect(0, HEADER_H + 1, tft->width(), tft->height() - HEADER_H - 1, COL_BG);

    if (menu.isOpen() && menu.inSlider()) {
        drawMenuSlider();
    } else if (menu.isOpen()) {
        drawMenuList();
    } else {
        switch (page) {
            case UiPage::RADIO: drawRadioPage(); break;
            case UiPage::CHANNEL: drawChannelPage(); break;
            case UiPage::GPS: drawGpsPage(); break;
            case UiPage::SYSTEM: drawSystemPage(); break;
            default: break;
        }
    }

    // Footer status (carousel only) and toast are both drawn last,
    // regardless of branch, so a toast fired from inside the menu is
    // visible immediately, and the footer status reappears the instant a
    // toast clears rather than waiting on the next periodic redraw.
    drawFooterStatus();
    drawToast();
}

// No fillScreen() here: drawPage() already unconditionally wipes the whole
// content region and redraws every element on every call (it has to, since
// pages don't track their own prior state), and the caller always follows
// a page change with a fullRedraw() in the same loop iteration (uiTask()
// below). An explicit clear here was a second full-panel blank stacked
// right before drawPage()'s own — pure redundant SPI traffic, and the
// direct cause of the visible black flash on every page change (bench
// feedback, 2026-08-25 hardware pass).
void nextPage() {
    page = (UiPage)(((uint8_t)page + 1) % (uint8_t)UiPage::COUNT);
    lastPageChange = millis();
}

void prevPage() {
    page = (UiPage)(((uint8_t)page + (uint8_t)UiPage::COUNT - 1) % (uint8_t)UiPage::COUNT);
    lastPageChange = millis();
}

void jumpToPage(UiPage p) {
    page = p;
    lastPageChange = millis();
}

// Performs the actual toggle/switch behind a fired MenuAction and confirms
// it via the toast layer — the same radio_task.h/wifi_task.h/logger_task.h
// calls Phase 3/4/5 already made, just no longer inlined into the menu's
// key-handling switch (see uiTask() below, where MenuState.handle()'s
// return value is routed here instead).
void fireMenuAction(MenuAction action) {
    char msg[48];
    switch (action) {
        case MenuAction::SELECT_MESHTASTIC:
        case MenuAction::SELECT_MESHCORE: {
            // Same one-loop-iteration-of-lag caveat Phase 4/5 already
            // documented: this queues the switch, it doesn't apply it —
            // the header/menu still show the outgoing profile until
            // radio_task's own loop picks the request up, typically the
            // next redraw tick. Direct target switch, not a cycle —
            // picking Meshtastic vs. MeshCore is a distinct selection
            // inside Profile's group, not "whichever one isn't active."
            const MissionProfile target = (action == MenuAction::SELECT_MESHTASTIC)
                                               ? MissionProfile::MESHTASTIC
                                               : MissionProfile::MESHCORE;
            radioRequestProfileSwitch(target);
            snprintf(msg, sizeof(msg), "Profile: %s", uiProfileLabel(target));
            showToast(msg);
            break;
        }
        case MenuAction::WIFI_TOGGLE: {
            // wifiToggle() only flips a *requested* flag — wifiTask (Core
            // 0) hasn't actually called softAP()/softAPdisconnect() yet by
            // the time this function returns, so wifiIsEnabled() read
            // right after wifiToggle() still reports the OLD state (same
            // one-loop-iteration-of-lag radioRequestProfileSwitch() already
            // has). Reading the pre-toggle state and negating it — instead
            // of re-querying post-toggle — is what SELECT_MESHTASTIC/
            // SELECT_MESHCORE above already do correctly by reporting the
            // requested target rather than the not-yet-applied live state;
            // this case just hadn't followed that pattern. Bug: toasted
            // "WiFi OFF" on the request that turned it on, and vice versa
            // (caught on hardware, 2026-08-25).
            const bool turningOn = !wifiIsEnabled();
            wifiToggle();
            if (turningOn) {
                char ssid[32];
                wifiApSsid(ssid, sizeof(ssid));
                snprintf(msg, sizeof(msg), "WiFi ON: %s", ssid);
            } else {
                snprintf(msg, sizeof(msg), "WiFi OFF");
            }
            showToast(msg);
            break;
        }
        case MenuAction::DEBUG_TOGGLE:
            loggerDebugToggle();
            showToast(loggerDebugIsEnabled() ? "Debug ON" : "Debug OFF");
            break;
        case MenuAction::TRACE_TOGGLE: {
            // Same pre-toggle-state-then-negate pattern as WIFI_TOGGLE above
            // (and the same reason): radioRequestTracePause() only queues
            // the request, radioIsTracePaused() doesn't reflect it until
            // the radio task's own loop picks it up — re-querying right
            // after the request would show the state being left, not
            // entered, same bug already fixed once this session for WiFi.
            const bool pausing = !radioIsTracePaused();
            radioRequestTracePause(pausing);
            showToast(pausing ? "Trace: STANDBY" : "Trace: ACTIVE");
            break;
        }
        case MenuAction::BRIGHTNESS_UP:
        case MenuAction::BRIGHTNESS_DOWN: {
            // Live-applied every step, like scrubbing any real slider —
            // but deliberately NOT saved to SD here. Saving on every step
            // would hammer the card if someone holds the key down; the
            // save happens once, on BACK out of the slider (see uiTask()
            // below), the same debounce point a "confirm" button would
            // give a form.
            int16_t next = (int16_t)activeBrightnessPercent +
                            (action == MenuAction::BRIGHTNESS_UP ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP);
            if (next < BRIGHTNESS_MIN) next = BRIGHTNESS_MIN;
            if (next > BRIGHTNESS_MAX) next = BRIGHTNESS_MAX;
            activeBrightnessPercent = (uint8_t)next;
            // Adjusting the slider always shows the live result
            // immediately, undimming if the display happened to be idle-
            // dimmed — the operator just acted on the keyboard, so it
            // can't still be "idle" the instant after this fires.
            displayDimmed = false;
            backlightSetPercent(activeBrightnessPercent);
            break; // no toast — the slider screen's own live "NN%" readout is the feedback
        }
        case MenuAction::IDLE_TIMEOUT_CYCLE: {
            // Plain local state this file owns directly (not an async
            // cross-task flag like WiFi's apActive), so there's no
            // pre/post-toggle staleness risk reading it right after
            // advancing it.
            idleTimeoutIndex = (uint8_t)((idleTimeoutIndex + 1) % IDLE_TIMEOUT_OPTION_COUNT);
            snprintf(msg, sizeof(msg), "Idle dim: %s", IDLE_TIMEOUT_OPTIONS[idleTimeoutIndex].label);
            showToast(msg);
            // One write per press — a discrete, deliberate tap, not a
            // continuous scrub, so this doesn't need the slider's
            // save-on-exit debounce.
            DisplaySettings settings;
            settings.brightness_pct = activeBrightnessPercent;
            settings.idle_timeout_index = idleTimeoutIndex;
            writeDisplaySettingsToSD(settings);
            break;
        }
        case MenuAction::NONE:
        default:
            break;
    }
}

// Drains the TCA8418 event FIFO and returns the most recently recognized
// KeyAction this poll (keyboard.h), or NONE. Several actions queued between
// polls collapse to the last one — an accepted imprecision at a 30ms poll
// interval for sparse, deliberate keypresses, the same tolerance the old
// gesture code documented for simultaneous keys.
KeyAction pollKeyAction() {
    if (!keyboardReady) return KeyAction::NONE;
    KeyAction result = KeyAction::NONE;
    while (keys.available() > 0) {
        const KeyAction a = keyboardDecodeEvent((uint8_t)keys.getEvent());
        if (a != KeyAction::NONE) result = a;
    }
    return result;
}

// NOTE: no startWrite()/endWrite() batching here on purpose, despite
// looking like the obvious next step. Arduino_GFX's fillRect()/print()
// etc. already each wrap themselves in their own startWrite()/endWrite()
// pair, and Arduino_HWSPI's is a straight SPIClass::beginTransaction()/
// endTransaction() call with a plain, non-recursive lock — a second,
// outer startWrite() around a sequence of calls that each take it again
// internally deadlocks on the first nested call (verified against the
// vendored GFX/SPI sources, not run on hardware — caught before it became
// a hang on first boot). That's moot now anyway: tft->flush() below is the
// real single-transaction boundary (Arduino_TFT::drawIndexedBitmap — one
// startWrite()/writeIndexedPixels()/endWrite() sequence over the whole
// composed frame), and it isn't nested inside anything.
void fullRedraw() {
    drawHeader();
    drawPage();
    tft->flush();
}

void uiTask(void *) {
    fullRedraw();

    uint32_t lastRedraw = millis();
    lastPageChange = lastRedraw;
    lastKeyActivity = lastRedraw;
    uint32_t lastRxSeen = radioPacketCount();
    bool wasAnimating = false;

    for (;;) {
        const KeyAction action = pollKeyAction();
        bool redraw = false;

        // Brightness idle-dim: any recognized key both resets the idle
        // clock and immediately undims if the display was dimmed — an
        // operator who just pressed something can't still be "idle" the
        // instant after. Checked before the carousel/menu dispatch below
        // so a keypress that also does something else (page change, menu
        // action) still counts as activity. idleTimeoutIndex == 0 ("Off")
        // disables idle-dim entirely, same meaning the old AUTODIM_TOGGLE
        // == false had.
        const uint32_t idleTimeoutMs = IDLE_TIMEOUT_OPTIONS[idleTimeoutIndex].ms;
        if (action != KeyAction::NONE) {
            lastKeyActivity = millis();
            if (displayDimmed) {
                displayDimmed = false;
                backlightSetPercent(activeBrightnessPercent);
            }
        } else if (idleTimeoutIndex != 0 && !displayDimmed &&
                   millis() - lastKeyActivity >= idleTimeoutMs) {
            displayDimmed = true;
            backlightSetPercent(idleDimTargetPercent());
        }

        // Detect new RX activity every loop, independent of whether a key
        // was pressed — this is what actually drives the header pulse dot
        // and RADIO's flash bar.
        const uint32_t rxNow = radioPacketCount();
        if (rxNow != lastRxSeen) {
            lastRxSeen = rxNow;
            rxPulseUntil = millis() + RX_PULSE_MS;
        }

        if (!menu.isOpen()) {
            // Carousel: page navigation is this file's own concern, not
            // MenuState's (ui_menu.h stays free of any UiPage dependency).
            if (action == KeyAction::PREV) {
                prevPage();
                redraw = true;
            } else if (action == KeyAction::NEXT) {
                nextPage();
                redraw = true;
            } else if (action == KeyAction::BACK) {
                menu.open();
                redraw = true;
            } else if (action == KeyAction::JUMP_1) {
                jumpToPage(UiPage::RADIO);
                redraw = true;
            } else if (action == KeyAction::JUMP_2) {
                jumpToPage(UiPage::CHANNEL);
                redraw = true;
            } else if (action == KeyAction::JUMP_3) {
                jumpToPage(UiPage::GPS);
                redraw = true;
            } else if (action == KeyAction::JUMP_4) {
                jumpToPage(UiPage::SYSTEM);
                redraw = true;
            }
            // JUMP_5 has no target now — WIFI folded into SYSTEM (Phase 6
            // UI redesign), so it's ignored the same way JUMP_1..5 are
            // already ignored inside the menu.
            // SELECT (Enter) is a no-op here — bench feedback 2026-08-24
            // found opening the menu with Enter felt wrong, since Enter's
            // role inside the menu is committing the highlighted change;
            // ESC (BACK) opens the menu instead, so the same key that
            // closes it also opens it.
        } else if (action != KeyAction::NONE) {
            // Menu open (root/group/slider level) — MenuState owns
            // navigation and level transitions; this file only reacts to
            // what fired. Captured before handle() runs: leaving the
            // Brightness slider (BACK, SLIDER -> ROOT) is the debounce
            // point for persisting it — see BRIGHTNESS_UP/DOWN's own
            // comment in fireMenuAction() for why saves don't happen on
            // every step instead.
            const bool leavingSlider = menu.inSlider() && action == KeyAction::BACK;
            const MenuAction fired = menu.handle(action);
            if (fired != MenuAction::NONE) fireMenuAction(fired);
            if (leavingSlider) {
                DisplaySettings settings;
                settings.brightness_pct = activeBrightnessPercent;
                settings.idle_timeout_index = idleTimeoutIndex;
                writeDisplaySettingsToSD(settings);
            }
            redraw = true;
        }

        const bool animating = (toastMsg[0] != '\0' && toastActive()) || rxPulseActive();
        const uint32_t redrawInterval = animating ? FAST_REDRAW_MS : REDRAW_MS;

        // Every tick below always goes through fullRedraw(), including the
        // FAST_REDRAW_MS animation burst — unlike direct-to-panel drawing,
        // redrawing "too much" into the off-screen canvas costs nothing the
        // viewer can see, since tft->flush() is the only point anything
        // reaches the glass, as one atomic blit. A leaner toast-only path
        // that skipped drawPage() briefly existed here before the canvas;
        // it existed purely to avoid a *visible* partial redraw, which
        // isn't a concern any more, so it was more code for no remaining
        // benefit.
        if (redraw) {
            fullRedraw();
            lastRedraw = millis();
        } else if (!menu.isOpen() && !keyboardReady && millis() - lastPageChange >= AUTO_ADVANCE_MS) {
            nextPage();
            fullRedraw();
            lastRedraw = millis();
        } else if (millis() - lastRedraw >= redrawInterval) {
            fullRedraw();
            lastRedraw = millis();
        } else if (wasAnimating && !animating) {
            // The toast or RX pulse just expired since the last redraw —
            // force one more full pass so its overlay actually clears
            // rather than lingering until the next periodic REDRAW_MS tick
            // (up to ~1s stale).
            fullRedraw();
            lastRedraw = millis();
        }
        wasAnimating = animating;

        if (toastMsg[0] != '\0' && !toastActive()) {
            toastMsg[0] = '\0';
        }

        // Poll rather than use the INT pin on GPIO11. At 30ms a keypress
        // feels immediate, and polling keeps this task free of an ISR that
        // would need its own I2C access — I2C is not interrupt-safe.
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

} // namespace

bool uiTaskStart(Arduino_GFX *gfx, const DisplaySettings &settings) {
    if (gfx == nullptr) return false;

    // Seed from main.cpp's boot-time SD load (display_settings.h) instead
    // of this file's hardcoded defaults — clamped defensively even though
    // display_settings.cpp already validates on load, since these values
    // go straight into backlightSetPercent() below. A brand-new/empty SD
    // card leaves `settings` at its own struct defaults (100%, 60s), which
    // is the same effective behavior this feature originally shipped with.
    activeBrightnessPercent = settings.brightness_pct;
    if (activeBrightnessPercent < BRIGHTNESS_MIN) activeBrightnessPercent = BRIGHTNESS_MIN;
    if (activeBrightnessPercent > BRIGHTNESS_MAX) activeBrightnessPercent = BRIGHTNESS_MAX;
    idleTimeoutIndex = settings.idle_timeout_index;
    if (idleTimeoutIndex >= IDLE_TIMEOUT_OPTION_COUNT) idleTimeoutIndex = 2;
    // main.cpp's backlightInit() (called before this, for the boot splash)
    // always starts at 100% — apply the real loaded level now so it's
    // visible from ui_task's very first frame instead of staying at 100%
    // until the operator happens to touch the Brightness slider.
    backlightSetPercent(activeBrightnessPercent);

    // Phase 6 bench pass (2026-08-25) found direct-to-panel drawing causes
    // real, visible flicker and tearing: every fillRect()/print() call is
    // immediately visible on the glass, so any redraw shows a blank-then-
    // redrawn flash to the eye even for values that didn't actually change,
    // and the toast's fast-redraw burst tore for the same reason. Fixed
    // the way M5PORKCHOP-style M5GFX/LovyanGFX sprite UIs get their
    // smoothness: everything above draws into this off-screen canvas
    // instead of the panel, and nothing reaches the glass until
    // tft->flush() blits the whole composed frame in one shot
    // (Arduino_TFT::drawIndexedBitmap — a single startWrite()/
    // writeIndexedPixels()/endWrite() sequence, confirmed against the
    // vendored GFX source). _Indexed rather than the full RGB565 canvas:
    // this UI only ever uses 6 colours (COL_BG/FG/DIM/GOOD/WARN/BAD, all
    // below), so 1 byte/pixel loses nothing and costs ~32KB
    // (240*135) instead of RGB565's ~63KB (240*135*2) — half the resource
    // commitment for a UI that never needed more than 6 distinct colours.
    // No PSRAM on this board (DESIGN.md §1), so this is a real malloc()
    // against the same heap budget as everything else, not a static
    // allocation — a deliberate one-time ~32KB tradeoff, decided with the
    // operator rather than assumed, given this reverses what was
    // previously a deliberate "no canvas/framebuffer" choice. Falls back
    // to drawing straight on the panel (the original behavior, flicker and
    // all) if the allocation fails, rather than taking the whole UI down
    // over ~32KB of missing heap headroom.
    canvas = new Arduino_Canvas_Indexed(gfx->width(), gfx->height(), gfx, 0, 0, 0);
    if (canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        tft = canvas;
    } else {
        delete canvas;
        canvas = nullptr;
        tft = gfx;
    }

    // Wire is already up from ioExpanderInit(); begin() again is harmless
    // and keeps this call self-contained if the boot order ever changes.
    Wire.begin(PIN_IOEXP_SDA, PIN_IOEXP_SCL);
    keyboardReady = keys.begin(KEYBOARD_I2C_ADDR, &Wire);
    if (keyboardReady) {
        keys.matrix(KEYBOARD_MATRIX_ROWS, KEYBOARD_MATRIX_COLS);
        keys.flush(); // discard boot-time noise
    }

    BaseType_t ok = xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 1, nullptr, 0);
    return ok == pdPASS;
}

bool uiKeyboardReady() {
    return keyboardReady;
}
