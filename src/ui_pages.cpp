// LoRaTrace RX — ui_task's status-page and menu/toast drawing.
//
// Split out of ui_task.cpp (2026-08-25 cleanup pass, PROGRESS.md/CLAUDE.md)
// — everything that draws to `uiTft` (RADIO/CHANNEL/GPS/SYSTEM, the header/
// footer chrome, the grouped menu, and the toast overlay) lives here, kept
// separate from menu-action business logic (ui_actions.cpp) and task
// lifecycle/input/loop (ui_task.cpp). See ui_task_shared.h for the state
// this file reads (uiTft, page, menu, toast/brightness/idle-dim fields) and
// ui_task.h for the subsystem's overall design.

#include "ui_task_shared.h"

#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "detection.h"
#include "gps_task.h"
#include "logger_task.h"
#include "radio_task.h"
#include "spi_bus.h"
#include "ui_labels.h"
#include "version.h"
#include "wifi_task.h"

namespace {

// 240x135 at rotation 1. Text size 1 is 6x8px; size 2 is 12x16px.
constexpr int16_t HEADER_H = 12;
constexpr uint16_t COL_BG = 0x0000;     // black
constexpr uint16_t COL_FG = 0xFFFF;     // white
constexpr uint16_t COL_DIM = 0x8410;    // grey
constexpr uint16_t COL_GOOD = 0x07E0;   // green
constexpr uint16_t COL_WARN = 0xFFE0;   // yellow
constexpr uint16_t COL_BAD = 0xF800;    // red

// ~512KB is the ESP32-S3FN8's total SRAM with no PSRAM (DESIGN.md §1) — an
// upper bound for context, used by both the heap bar and its colour tiers.
constexpr uint32_t HEAP_BUDGET_KB = 512;

// Toast geometry only used within drawToast() below — TOAST_DURATION_MS
// itself is shared (ui_task_shared.h) since toastActive()/showToast() in
// ui_task.cpp also need it.
constexpr uint32_t TOAST_SLIDE_MS = 150;
constexpr int16_t TOAST_H = 16;

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
    const int16_t x = uiTft->width() - w - 4;
    const int16_t y = 2;

    uiTft->fillRect(x - 30, y - 1, w + 34, h + 2, COL_BG);

    if (mv == 0) {
        // Unknown, not empty. Drawing 0% would imply a dying battery when
        // the truth is the ADC gave an implausible reading (USB-only, no
        // cell fitted, etc.).
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(x - 28, y);
        uiTft->print("bat ?");
        return;
    }

    const uint8_t pct = batteryPercentFromMv(mv);
    const uint16_t colour = (pct >= 50) ? COL_GOOD : (pct >= 20 ? COL_WARN : COL_BAD);

    uiTft->setTextSize(1);
    uiTft->setTextColor(colour, COL_BG);
    uiTft->setCursor(x - 30, y);
    uiTft->print(pct);
    uiTft->print('%');

    uiTft->drawRect(x, y, w, h, colour);
    uiTft->fillRect(x + w, y + 2, 2, h - 4, colour); // terminal nub
    const int16_t fill = (int16_t)((w - 2) * pct / 100);
    if (fill > 0) uiTft->fillRect(x + 1, y + 1, fill, h - 2, colour);
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

// Persistent status line in the footer band (Phase 6) — profile
// left-anchored, page position right-anchored. Carousel only: the menu
// already shows the active profile on its own "Profile" root row, and
// the header breadcrumb already says which group is open. Drawn before
// drawToast() so a toast simply paints over it for its duration and this
// reappears once the toast clears.
void drawFooterStatus() {
    if (menu.isOpen()) return;
    const int16_t y = uiTft->height() - 10;
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);

    uiTft->setCursor(2, y);
    uiTft->print(uiProfileLabel(radioActiveProfile()));

    char posBuf[8];
    snprintf(posBuf, sizeof(posBuf), "%u/%u", (unsigned)page + 1, (unsigned)UiPage::COUNT);
    uiTft->setCursor(uiTft->width() - (int16_t)strlen(posBuf) * 6 - 2, y);
    uiTft->print(posBuf);
}

// A small "label above value" block, used for the secondary/context column
// every redesigned page below carries alongside its primary left-column
// numbers — Phase 6's answer to the old single-column layout's unused right
// half (PROGRESS.md 2026-08-25 Decisions log). One shared helper instead of
// each page inventing its own right-column formatting, since every page
// now does this.
void statBlock(int16_t x, int16_t y, const char *label, const char *value, uint16_t valueColour = COL_FG) {
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(x, y);
    uiTft->print(label);
    uiTft->setTextColor(valueColour, COL_BG);
    uiTft->setCursor(x, y + 9);
    uiTft->print(value);
}

void drawRadioPage() {
    const uint32_t drops = radioQueueDropCount() + loggerRowsDropped();
    char buf[16];

    // Left column — the three numbers an operator checks first.
    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 6);
    uiTft->print("rx ");
    uiTft->print(radioPacketCount());

    uiTft->setCursor(2, HEADER_H + 26);
    uiTft->print("log ");
    uiTft->print(loggerRowsWritten());
    // RX activity pulse — solid flash under "log" while a detection was
    // heard in the last RX_PULSE_MS; reverts on its own because drawPage()
    // clears this whole region before every redraw.
    if (rxPulseActive()) {
        uiTft->fillRect(2, HEADER_H + 23, 58, 2, COL_GOOD);
    }

    // Drops are the number that decides whether the architecture is holding
    // up under real traffic, so they get colour rather than being buried.
    uiTft->setTextColor(drops == 0 ? COL_GOOD : COL_BAD, COL_BG);
    uiTft->setCursor(2, HEADER_H + 46);
    uiTft->print("drop ");
    uiTft->print(drops);

    // STANDBY banner (Trace pause/standby, System menu): rx/log/drop above
    // stay as real frozen totals rather than being replaced — the thing
    // that must not be ambiguous is "is the radio actually listening right
    // now," not the counters themselves, which are still meaningful while
    // paused. Sits in the gap between the hero column and the bottom flush
    // band, which RADIO's layout otherwise leaves empty (unlike GPS/SYSTEM,
    // which use it for their own secondary content).
    if (radioIsTracePaused()) {
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 66);
        uiTft->print("STANDBY");
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
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 94);
    uiTft->print("flush ");
    uiTft->print(loggerFlushCount());
    uiTft->print("  max ");
    uiTft->print(loggerMaxFlushMs());
    uiTft->print("ms");
}

// Track + marker, not a fill bar — frequency is a *position* within the
// module's tuned range, not a proportion of something used up (unlike
// drawHeapBar() below). 868-923MHz is the SX1262 front end's actual tuned
// range (DESIGN.md §1), not the full 902-928MHz US ISM band — the point is
// showing real margin to that edge, not just an arbitrary axis.
void drawFreqBar(int16_t x, int16_t y, int16_t w, float freqMhz) {
    constexpr float LO = 868.0f, HI = 923.0f;
    uiTft->drawFastHLine(x, y, w, COL_DIM);
    float frac = (freqMhz - LO) / (HI - LO);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t mx = x + (int16_t)((w - 3) * frac);
    uiTft->fillRect(mx, y - 3, 3, 7, COL_GOOD);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(x, y + 6);
    uiTft->print((int)LO);
    char hiBuf[8];
    snprintf(hiBuf, sizeof(hiBuf), "%d", (int)HI);
    uiTft->setCursor(x + w - (int16_t)strlen(hiBuf) * 6, y + 6);
    uiTft->print(hiBuf);
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

    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 6);
    uiTft->print(ch.freq_mhz, 3);
    uiTft->print(" MHz");

    uiTft->setCursor(2, HEADER_H + 32);
    uiTft->print("SF");
    uiTft->print(ch.sf);
    uiTft->print(" BW");
    uiTft->print(ch.bw_khz, 1);

    drawFreqBar(2, HEADER_H + 62, 108, ch.freq_mhz);

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 94);
    uiTft->print("CR4/");
    uiTft->print(ch.cr_denom);
    uiTft->print("  sync 0x");
    uiTft->print(ch.sync_word, HEX);

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
        uiTft->fillRect(bx, y + h - barH, BAR_W, barH, COL_GOOD);
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(bx, y + h + 3);
        uiTft->print(fix.talkers[i].id);
    }
}

void drawGpsPage() {
    GpsFix fix;
    const bool have = gpsGetFix(fix, pdMS_TO_TICKS(100));

    uiTft->setTextSize(2);
    if (have && fix.has_position) {
        uiTft->setTextColor(COL_GOOD, COL_BG);
        uiTft->setCursor(2, HEADER_H + 6);
        uiTft->print(fix.fix_type >= 3 ? "3D FIX" : "2D FIX");

        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->setCursor(2, HEADER_H + 28);
        uiTft->print(fix.lat, 5);
        uiTft->setCursor(2, HEADER_H + 48);
        uiTft->print(fix.lon, 5);

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
        uiTft->setTextColor(fix.sats_in_view > 0 ? COL_WARN : COL_BAD, COL_BG);
        uiTft->setCursor(2, HEADER_H + 6);
        uiTft->print("NO FIX");

        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->setCursor(2, HEADER_H + 30);
        uiTft->print("view ");
        uiTft->print(fix.sats_in_view);

        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 52);
        uiTft->print(fix.sats_in_view > 0 ? "acquiring, keep still" : "no sky - go outside");
    }

    // Right column, under sats/qual (or under NO FIX in the no-fix branch,
    // which otherwise left that whole side empty) — moved here from the
    // old dim text line in the left column's open width, and other pages
    // now share this same 170..240 zone (bench feedback, PROGRESS.md
    // 2026-08-25 Decisions log).
    drawConstellationBars(170, HEADER_H + 58, 14, fix);

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 72);
    if (have && fix.has_time) {
        char ts[24];
        detectionFormatTimestamp(ts, sizeof(ts), true, fix.year, fix.month, fix.day, fix.hour,
                                 fix.minute, fix.second);
        uiTft->print(ts);
    } else {
        uiTft->print("nmea ");
        uiTft->print(gpsSentenceCount());
        uiTft->print(" crc ");
        uiTft->print(gpsChecksumErrorCount());
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
    uiTft->drawRect(x, y, w, h, colour);
    const uint32_t usedK = (freeHeapK < HEAP_BUDGET_KB) ? (HEAP_BUDGET_KB - freeHeapK) : HEAP_BUDGET_KB;
    float frac = (float)usedK / (float)HEAP_BUDGET_KB;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t fill = (int16_t)((w - 2) * frac);
    if (fill > 0) uiTft->fillRect(x + 1, y + 1, fill, h - 2, colour);
}

// Merged with the old WIFI carousel page (Phase 6 UI redesign, 2026-08-25):
// AP on/off plus client count didn't need a whole carousel slot once SYSTEM
// had room for a 2x2 grid. SSID moved into the toast message WIFI_TOGGLE
// fires (fireMenuAction() in ui_actions.cpp) instead of living on this
// page, which now shows only ON/OFF + client count. Reworked once already
// the same day after the merge first shipped: the stat column sat far
// right of the hero numbers with a wide empty gap, and dead space remained
// below everything. Now a 2x2 grid pulled in closer to the hero column,
// and a heap bar fills what used to be blank space under "k heap" with an
// actual gauge instead of just relocating the same lines of text.
void drawSystemPage() {
    char buf[16];

    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 6);
    uiTft->print(millis() / 60000);
    uiTft->print(" min");

    const uint32_t heap = ESP.getFreeHeap();
    const uint16_t heapColour = heapUsageColour(heap / 1024);
    uiTft->setTextColor(heapColour, COL_BG);
    uiTft->setCursor(2, HEADER_H + 28);
    uiTft->print(heap / 1024);
    uiTft->print("k heap");
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
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 66);
    uiTft->print("keys ");
    uiTft->print(keyboardReady ? "tca8418" : "none (auto)");
    uiTft->print("  health ");
    uiTft->print(loggerSessionRows());

    uiTft->setCursor(2, HEADER_H + 78);
    uiTft->print(FIRMWARE_VERSION);
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
    uiTft->fillRect(0, y - 3, uiTft->width(), 20, bg);
    uiTft->setTextSize(2);
    uiTft->setTextColor(fg, bg);
    uiTft->setCursor(4, y);
    uiTft->print(rowLabel);
    if (value != nullptr && value[0] != '\0') {
        uiTft->print(' ');
        uiTft->print(value);
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

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, uiTft->height() - 9);
    uiTft->print(",/. move   Enter select   ` back");
}

// Brightness slider screen (2026-08-25) — the one SLIDER view today. Large
// live readout plus a filled-bar track, same outline+fill visual language
// drawHeapBar()/drawFreqBar() already use elsewhere in this file rather
// than inventing a third bar style.
void drawMenuSlider() {
    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 10);
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)activeBrightnessPercent);
    uiTft->print(pctBuf);

    constexpr int16_t BAR_X = 2, BAR_Y = HEADER_H + 40, BAR_W = 200, BAR_H = 14;
    uiTft->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_GOOD);
    const float frac = (float)(activeBrightnessPercent - BRIGHTNESS_MIN) / (float)(BRIGHTNESS_MAX - BRIGHTNESS_MIN);
    const int16_t fill = (int16_t)((BAR_W - 2) * frac);
    if (fill > 0) uiTft->fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, COL_GOOD);

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, uiTft->height() - 9);
    uiTft->print(",/. adjust +/-5%   ` back");
}

// Toast overlay (Phase 6) — flush-bottom band that slides up from
// off-panel on show and carries a shrinking countdown bar along its own
// bottom edge. Both are just per-frame rectangle geometry (no alpha
// blending needed, unlike the design mockup's decaying RX-pulse glow),
// driven by the bounded fast-redraw burst in ui_task.cpp's uiTask() while
// toastActive(), not a continuous animation loop. Drawn last, on top of
// whatever page/menu is showing.
void drawToast() {
    if (!toastActive()) return;
    const uint32_t elapsed = millis() - toastShownAt;
    const float slideT = elapsed >= TOAST_SLIDE_MS ? 1.0f : (float)elapsed / (float)TOAST_SLIDE_MS;
    const int16_t y = uiTft->height() - (int16_t)(TOAST_H * slideT);

    uiTft->fillRect(0, y, uiTft->width(), TOAST_H, COL_FG);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_BG, COL_FG);
    uiTft->setCursor(4, y + 4);
    uiTft->print(toastMsg);

    float remain = 1.0f - (float)elapsed / (float)TOAST_DURATION_MS;
    if (remain < 0.0f) remain = 0.0f;
    const int16_t barW = (int16_t)(uiTft->width() * remain);
    if (barW > 0) uiTft->fillRect(0, y + TOAST_H - 2, barW, 2, COL_DIM);
}

} // namespace

void drawHeader() {
    uiTft->fillRect(0, 0, uiTft->width(), HEADER_H, COL_BG);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, 2);

    if (menu.isOpen()) {
        uiTft->print("MENU");
        // Breadcrumb, e.g. "MENU > System > Display" — tells an operator
        // how deep they are without needing to back out to check. Full
        // ancestor chain now that nesting is real (2026-08-25) — not just
        // one group name, since a GROUP can itself open another GROUP.
        // "MENU > System > Display > Brightness" (worst case today, deepest
        // path) is 36 chars at size-1 text (6px/char = 216px), clear of
        // the 240px edge.
        uiTft->setTextColor(COL_DIM, COL_BG);
        for (uint8_t i = 0; i < menu.breadcrumbCount(); i++) {
            uiTft->print(" > ");
            uiTft->print(menu.breadcrumbLabel(i));
        }
        if (menu.inSlider()) {
            uiTft->print(" > ");
            uiTft->print(menu.currentItem().label);
        }
    } else {
        // Page name only — profile and page position moved to the footer
        // status line (drawFooterStatus() below) so this text never
        // crowds the status-dot cluster or the battery reading on either
        // side of it (bench feedback, PROGRESS.md 2026-08-25 Decisions
        // log).
        uiTft->print(pageName(page));
    }

    drawBattery();

    // Status dot cluster (Phase 6): GPS fix state, heap health, and RX
    // activity, always visible from any page instead of only their own
    // dedicated one. 5px clear of the battery reading (bench feedback) —
    // replaces the old idle heartbeat blink entirely.
    uiTft->fillCircle(157, 6, 2, gpsStatusColour());
    uiTft->fillCircle(166, 6, 2, heapStatusColour());
    uiTft->fillCircle(175, 6, 2, rxPulseActive() ? COL_GOOD : COL_DIM);

    uiTft->drawFastHLine(0, HEADER_H, uiTft->width(), COL_DIM);
}

void drawPage() {
    uiTft->fillRect(0, HEADER_H + 1, uiTft->width(), uiTft->height() - HEADER_H - 1, COL_BG);

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
