#include "ui_task.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "board_pins.h"
#include "detection.h"
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

Arduino_GFX *tft = nullptr;

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

// Phase 6 root table, revised twice 2026-08-25 (BRAND.md's "Revised again"
// note has the full rationale): both root rows are GROUP. "Profile" opens
// onto the real, technical profile names — Meshtastic, MeshCore today;
// Reticulum/Spectrum join once they have a real HOME_LISTEN table (Phase
// 8) — instead of cycling one at a time on Enter the way Phase 4/5 did.
// Deliberately not branded as "Mesh Trace" or similar: these are LoRa
// presets on one sniffer, not sibling products, and BRAND.md already had
// "Profile" as its preferred word for exactly this axis before an earlier
// same-day revision briefly tried a per-profile brand name instead.
// "System" still holds the WiFi/Debug toggles Phase 3/Phase-5-bench-day
// added. Two root rows is not a hardcoded ceiling — DISCOVERY_SWEEP (Phase
// 7) / ENERGY_SWEEP (Phase 8) each get their own row or group here without
// touching MenuState itself, which is the whole point of this redesign
// (ROADMAP.md Phase 6, PROGRESS.md 2026-08-25 Decisions log).
constexpr MenuEntry PROFILE_GROUP_ITEMS[] = {
    {"Meshtastic", MenuAction::SELECT_MESHTASTIC},
    {"MeshCore", MenuAction::SELECT_MESHCORE},
};
constexpr MenuEntry SYSTEM_GROUP_ITEMS[] = {
    {"WiFi", MenuAction::WIFI_TOGGLE},
    {"Debug", MenuAction::DEBUG_TOGGLE},
};
constexpr RootEntry ROOT_ITEMS[] = {
    {"Profile", RootKind::GROUP, MenuAction::NONE, PROFILE_GROUP_ITEMS, 2},
    {"System", RootKind::GROUP, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 2},
};
constexpr uint8_t ROOT_COUNT = 2;
MenuState menu(ROOT_ITEMS, ROOT_COUNT);

// Toast layer (Phase 6): a brief overlay message for feedback that isn't
// tied to whichever menu row happens to be highlighted — e.g. confirming a
// toggle fired right before BACK leaves the menu. No canvas/framebuffer
// involved (this project draws direct-to-panel — DESIGN.md §1's
// no-framebuffer rule stays intact): the only cost is this static buffer,
// not a dynamic allocation, so there's no heap number to gate behind the
// way WiFi's AP needed one.
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

// Heap-health header dot — same threshold drawSystemPage() already colors
// "k heap" by, just always visible instead of only on its own page.
uint16_t heapStatusColour() {
    return ESP.getFreeHeap() > 100000 ? COL_GOOD : COL_WARN;
}

void drawHeader() {
    tft->fillRect(0, 0, tft->width(), HEADER_H, COL_BG);
    tft->setTextSize(1);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, 2);

    if (menu.isOpen()) {
        tft->print("MENU");
        if (menu.level() == MenuLevel::GROUP) {
            // Breadcrumb, e.g. "MENU > System" — tells an operator which
            // group they're inside without needing to back out to check.
            tft->setTextColor(COL_DIM, COL_BG);
            tft->print(" > ");
            tft->print(menu.currentRoot().label);
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
void drawHeapBar(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t heapK, uint16_t colour) {
    tft->drawRect(x, y, w, h, colour);
    float frac = (float)heapK / 512.0f;
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
    const uint16_t heapColour = heap > 100000 ? COL_GOOD : COL_WARN;
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

// What a group row's value column shows. Generic over both groups
// (Profile's choices and System's toggles) — MenuState/RootEntry are
// data-driven on purpose (ui_menu.h), and this stays a single switch on
// MenuAction rather than one function per group, the same way
// drawMenuGroup() below stays one function for both.
const char *menuEntryValue(MenuAction action) {
    switch (action) {
        case MenuAction::SELECT_MESHTASTIC:
            return radioActiveProfile() == MissionProfile::MESHTASTIC ? "ACTIVE" : "";
        case MenuAction::SELECT_MESHCORE:
            return radioActiveProfile() == MissionProfile::MESHCORE ? "ACTIVE" : "";
        case MenuAction::WIFI_TOGGLE: return wifiIsEnabled() ? "ON" : "OFF";
        case MenuAction::DEBUG_TOGGLE: return loggerDebugIsEnabled() ? "ON" : "OFF";
        default: return "";
    }
}

void drawMenuRoot() {
    for (uint8_t i = 0; i < ROOT_COUNT; i++) {
        const RootEntry &r = ROOT_ITEMS[i];
        char value[24];
        if (r.groupItems == PROFILE_GROUP_ITEMS) {
            // Root row still surfaces the live profile (unlike System's
            // bare ">") so the active protocol reads without drilling
            // into the group.
            snprintf(value, sizeof(value), "%s >", uiProfileLabel(radioActiveProfile()));
        } else {
            snprintf(value, sizeof(value), ">");
        }
        drawMenuRow(HEADER_H + 10 + i * 24, r.label, value, menu.rootIndex() == i);
    }

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. move   Enter select   ` back");
}

void drawMenuGroup() {
    const RootEntry &r = menu.currentRoot();
    for (uint8_t i = 0; i < r.groupCount; i++) {
        const MenuEntry &e = r.groupItems[i];
        drawMenuRow(HEADER_H + 10 + i * 24, e.label, menuEntryValue(e.action), menu.groupIndex() == i);
    }

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. move   Enter act   ` back");
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

    if (menu.level() == MenuLevel::ROOT) {
        drawMenuRoot();
    } else if (menu.level() == MenuLevel::GROUP) {
        drawMenuGroup();
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

void nextPage() {
    page = (UiPage)(((uint8_t)page + 1) % (uint8_t)UiPage::COUNT);
    lastPageChange = millis();
    tft->fillScreen(COL_BG);
}

void prevPage() {
    page = (UiPage)(((uint8_t)page + (uint8_t)UiPage::COUNT - 1) % (uint8_t)UiPage::COUNT);
    lastPageChange = millis();
    tft->fillScreen(COL_BG);
}

void jumpToPage(UiPage p) {
    page = p;
    lastPageChange = millis();
    tft->fillScreen(COL_BG);
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
        case MenuAction::WIFI_TOGGLE:
            wifiToggle();
            if (wifiIsEnabled()) {
                char ssid[32];
                wifiApSsid(ssid, sizeof(ssid));
                snprintf(msg, sizeof(msg), "WiFi ON: %s", ssid);
            } else {
                snprintf(msg, sizeof(msg), "WiFi OFF");
            }
            showToast(msg);
            break;
        case MenuAction::DEBUG_TOGGLE:
            loggerDebugToggle();
            showToast(loggerDebugIsEnabled() ? "Debug ON" : "Debug OFF");
            break;
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

void uiTask(void *) {
    tft->fillScreen(COL_BG);
    drawHeader();
    drawPage();

    uint32_t lastRedraw = millis();
    lastPageChange = lastRedraw;
    uint32_t lastRxSeen = radioPacketCount();
    bool wasAnimating = false;

    for (;;) {
        const KeyAction action = pollKeyAction();
        bool redraw = false;

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
            // Menu open (root or group level) — MenuState owns navigation
            // and level transitions; this file only reacts to what fired.
            const MenuAction fired = menu.handle(action);
            if (fired != MenuAction::NONE) fireMenuAction(fired);
            redraw = true;
        }

        const bool animating = (toastMsg[0] != '\0' && toastActive()) || rxPulseActive();
        const uint32_t redrawInterval = animating ? FAST_REDRAW_MS : REDRAW_MS;

        if (redraw) {
            drawHeader();
            drawPage();
            lastRedraw = millis();
        } else if (!menu.isOpen() && !keyboardReady && millis() - lastPageChange >= AUTO_ADVANCE_MS) {
            nextPage();
            drawHeader();
            drawPage();
            lastRedraw = millis();
        } else if (millis() - lastRedraw >= redrawInterval) {
            drawHeader(); // battery + status dots (+ page name/footer via drawPage)
            drawPage();
            lastRedraw = millis();
        } else if (wasAnimating && !animating) {
            // The toast or RX pulse just expired since the last redraw —
            // force one more pass so its overlay actually clears rather
            // than lingering until the next periodic REDRAW_MS tick (up to
            // ~1s stale).
            drawHeader();
            drawPage();
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

bool uiTaskStart(Arduino_GFX *gfx) {
    if (gfx == nullptr) return false;
    tft = gfx;

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
