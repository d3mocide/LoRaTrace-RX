// LoRaTrace RX — ui_task's status-page and menu/toast drawing.
//
// Split out of ui_task.cpp (2026-08-25 cleanup pass) — everything that
// draws to `uiTft` (RADIO/CHANNEL/GPS/SYSTEM, header/footer chrome, the
// grouped menu, the toast overlay) lives here, separate from menu-action
// logic (ui_actions.cpp) and task lifecycle/input/loop (ui_task.cpp). See
// ui_task_shared.h for the state this file reads and ui_task.h for the
// subsystem design.

#include "ui_task_shared.h"

#include <stdio.h>
#include <string.h>

#include "analyzer_state.h"
#include "battery.h"
#include "capture_history.h"
#include "capture_settings.h"
#include "cell_plan.h"
#include "detection.h"
#include "discovery_plan.h"
#include "energy_plan.h"
#include "gps_task.h"
#include "logger_task.h"
#include "node_roster.h"
#include "scope_trace.h"
#include "serial_control.h"
#include "radio_task.h"
#include "spi_bus.h"
#include "ui_labels.h"
#include "version.h"
#include "waterfall.h"
#include "wifi_task.h"

namespace {

// 240x135 at rotation 1. Text size 1 is 6x8px; size 2 is 12x16px.
constexpr int16_t HEADER_H = 12;
// The header's status-dot cluster (fillCircle(157,...) below) is the
// actual nearest obstacle to breadcrumb text, not the battery gauge
// further right -- leftmost dot edge is real x=155 (157-radius 2), vs.
// the battery's own clear-rect at x=184. A first fix here (2026-08-29)
// checked only the battery and still collided with the dots on real
// hardware. Cursor starts at x=2, so 25 whole size-1 characters (150px)
// is the real safe budget, clear of x=155. Caught 4 levels deep in the
// Brightness slider ("MENU > System > Display > Brightness", 36 chars)
// and also on two 3-deep group lists whose own label is long enough to
// overrun this alone ("MENU > System > Connectivity", 28 chars; "MENU >
// System > Diagnostics", 27) -- not just the slider case.
constexpr size_t HEADER_BREADCRUMB_MAX_CHARS = 25;
constexpr uint16_t COL_BG = 0x0000;     // black
constexpr uint16_t COL_FG = 0xFFFF;     // white
constexpr uint16_t COL_DIM = 0xBDF7;    // light grey, ~75% brightness -- mid-grey (0x8410, ~51%)
                                         // was hard to read in direct sunlight (2026-08-29). Two
                                         // amber revisions followed (hue instead of brightness, to
                                         // dodge a worry that a brighter grey would look the same
                                         // as COL_FG's white under glare) but neither read as
                                         // genuinely "dim" once seen on real hardware -- reverted to
                                         // grey at operator request; the white-under-glare risk was
                                         // reasoning, never actually field-tested, so this is that
                                         // test. Revisit if it turns out to wash out in direct sun.
constexpr uint16_t COL_GOOD = 0x07E0;   // green
constexpr uint16_t COL_WARN = 0xFFE0;   // yellow
constexpr uint16_t COL_BAD = 0xF800;    // red

// ~512KB is the ESP32-S3FN8's total SRAM with no PSRAM (docs/DESIGN.md §1) — an
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
        case UiPage::ACTIVITY: return "ACTIVITY";
        case UiPage::CHANNEL: return "CHANNEL";
        case UiPage::GPS: return "GPS";
        case UiPage::SYSTEM: return "SYSTEM";
        case UiPage::PROBE: return "PROBE";
        case UiPage::SWEEP: return "SWEEP";
        case UiPage::CELL: return "CELL";
        case UiPage::METER: return "METER";
        case UiPage::WATERFALL: return "WATERFALL";
        case UiPage::SCOPE: return "SCOPE";
        case UiPage::CAPTURES: return "CAPTURES";
        case UiPage::NODES: return "NODES";
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

    // Right-aligned against the gauge with a fixed 2px gap, not a fixed
    // left-anchor offset: a fixed cursor at x-30 let "7%"'s left edge sit
    // wherever three fewer characters happened to land, so the number
    // visually drifted away from the gauge for low/short values instead of
    // always hugging it (operator feedback, 2026-08-29). Size-1 glyphs are
    // a fixed 6px wide, so the width is exact, not a measurement guess.
    const uint8_t digits = (pct >= 100) ? 3 : (pct >= 10 ? 2 : 1);
    const int16_t textWidth = (int16_t)((digits + 1) * 6); // +1 for '%'
    uiTft->setTextSize(1);
    uiTft->setTextColor(colour, COL_BG);
    uiTft->setCursor(x - 2 - textWidth, y);
    uiTft->print(pct);
    uiTft->print('%');

    uiTft->drawRect(x, y, w, h, colour);
    uiTft->fillRect(x + w, y + 2, 2, h - 4, colour); // terminal nub
    const int16_t fill = (int16_t)((w - 2) * pct / 100);
    if (fill > 0) uiTft->fillRect(x + 1, y + 1, fill, h - 2, colour);
}

// GPS-fix header dot. Non-blocking mutex try, keeps the last known colour
// when busy rather than blocking — drawHeader() runs at FAST_REDRAW_MS
// during an active toast/pulse, and a dot only needs to be roughly
// current, not per-frame exact.
uint16_t gpsStatusColour() {
    static uint16_t cached = COL_DIM;
    GpsFix fix;
    if (gpsGetFix(fix, 0)) {
        cached = fix.has_position ? COL_GOOD : (fix.sats_in_view > 0 ? COL_WARN : COL_BAD);
    }
    return cached;
}

// Shared 3-tier colour for every heap-usage display (header dot, SYSTEM's
// "k heap" text, and its bar) — green under 80% of the ~512KB no-PSRAM
// SRAM budget used, yellow 80-90%, red above 90%. Takes free heap in KB so
// SYSTEM's page doesn't redo the division.
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

// Persistent footer status — profile left-anchored, page position
// right-anchored. Carousel only: the menu already shows the active
// profile on its own "Profile" row. Drawn before drawToast() so a toast
// paints over it and it reappears once the toast clears.
//
// Position/total come from mainCarouselPosition()/mainCarouselCount()
// (ui_task.cpp), not raw UiPage ordinals/UiPage::COUNT: this project's
// operator-facing carousel is four stops (Radio/Channel/GPS/System) since
// Tools/Analyze moved into the menu (2026-09-05) — mainCarouselPosition()
// returns 0 while on one of their sub-pages (Probe/Sweep/Cell, Meter/
// Waterfall/Scope/Captures/Nodes), reached only through the menu now and
// with no carousel position of their own, so the "N/M" text is omitted
// entirely rather than showing a stale or misleading number. An earlier
// revision used raw UiPage ordinals for a six-stop carousel and showed
// e.g. Analyze as "8/14" — technically not wrong, but confusing enough
// that an operator asked "where are the other 6 cards" (2026-09-04).
void drawFooterStatus() {
    if (menu.isOpen()) return;
    const int16_t y = uiTft->height() - 10;
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);

    uiTft->setCursor(2, y);
    uiTft->print(uiProfileLabel(radioActiveProfile()));

    const uint8_t pos = mainCarouselPosition();
    if (pos != 0) {
        char posBuf[8];
        snprintf(posBuf, sizeof(posBuf), "%u/%u", (unsigned)pos, (unsigned)mainCarouselCount());
        uiTft->setCursor(uiTft->width() - (int16_t)strlen(posBuf) * 6 - 2, y);
        uiTft->print(posBuf);
    }
}

// "Label above value" block for the secondary/context column every page
// carries alongside its primary left-column numbers, so each page doesn't
// invent its own right-column formatting.
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
    const uint32_t drops = radioQueueDropCount() + loggerRowsDropped() + loggerScanRowsDropped();
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

    // STANDBY: the one piece of the old Probe/repeat-scan banner brought
    // back here (operator request, 2026-09-05) after Activity's own idle
    // view dropped its hero line and lost this as its last remaining home
    // — watch-paused is exactly the kind of thing this page's rx/log/drop
    // counters need it to not be silently ambiguous about. True whenever
    // the radio isn't actively listening, whether that's a manual pause or
    // a bounded action (Probe/Sweep/Cell/Scope) currently owning it — the
    // specific "which one and how far along" detail is Activity's job, not
    // this one line's.
    if (radioIsTracePaused()) {
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 66);
        uiTft->print("STANDBY");
    }

    // Right column, x=170 — kept consistent across every page so it
    // lands in the same physical 70px-wide zone rather than drifting.
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
    // BATCH_BUF_SIZE needs retuning (docs/DESIGN.md §8.2), not for a quick
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

void drawProbePage() {
    const DiscoverySweepState state = radioDiscoverySweepState();
    const uint8_t done = radioDiscoveryCandidateIndex();
    const uint8_t total = radioDiscoveryCandidateCount();
    const uint16_t hits = radioDiscoveryCadDetectedCount();
    const uint16_t free = radioDiscoveryCadFreeCount();
    const uint16_t timeouts = radioDiscoveryCadTimeoutCount();
    const uint16_t errors = radioDiscoveryErrorCount();

    uiTft->setTextSize(2);
    uiTft->setCursor(2, HEADER_H + 8);
    if (state == DiscoverySweepState::IDLE) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("NO PROBE YET");
        uiTft->setTextSize(1);
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print("P / Enter to run Probe");
        return;
    }

    const bool running = state == DiscoverySweepState::RUNNING;
    // After RESULT_HOLD_MS, revert the headline word to a dim IDLE so the
    // operator gets a clear "ready to run again" cue instead of a stale
    // COMPLETE sitting on screen indefinitely — everything below this
    // still reflects the real last result; only the headline changes.
    const bool holdExpired = !running && probeTerminalShownAt != 0 &&
                             millis() - probeTerminalShownAt >= RESULT_HOLD_MS;
    if (holdExpired) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("IDLE");
    } else {
        const uint16_t colour = state == DiscoverySweepState::FAILED
                                    ? COL_BAD
                                    : (running || state == DiscoverySweepState::CANCELLED ? COL_WARN : COL_GOOD);
        uiTft->setTextColor(colour, COL_BG);
        if (running) {
            uiTft->print("SCANNING");
        } else if (state == DiscoverySweepState::COMPLETE) {
            uiTft->print("COMPLETE");
        } else if (state == DiscoverySweepState::CANCELLED) {
            uiTft->print("CANCELLED");
        } else {
            uiTft->print("FAILED");
        }
    }

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 31);
    if (running) {
        uiTft->print("watch paused  ");
        uiTft->print(done);
        uiTft->print('/');
        uiTft->print(total);
        return; // nothing else to summarize until a candidate lands somewhere
    }
    uiTft->print("targets ");
    uiTft->print(done);
    uiTft->print('/');
    uiTft->print(total);
    uiTft->print("  away ");
    uiTft->print(radioDiscoveryLastAwayMs());
    uiTft->print("ms");

    // Plain-English headline instead of raw "cad hit"/"free" jargon — the
    // actual answer to "did I find anything", not internal counters.
    char summary[24];
    snprintf(summary, sizeof(summary), "%u/%u channels active", (unsigned)hits, (unsigned)total);
    uiTft->setTextColor(hits > 0 ? COL_WARN : COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 47);
    uiTft->print(summary);

    // Which named candidates actually hit. Mask bit i corresponds to
    // plan.candidates[i] — radio_task.cpp's own raw loop index into the
    // candidate table, not the skip-adjusted done/total counters above
    // (see radioDiscoveryCadDetectedMask()'s own doc comment).
    uiTft->setCursor(2, HEADER_H + 59);
    if (hits == 0) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("no activity heard");
    } else {
        const DiscoveryPlan plan = discoveryPlanForProfile(radioActiveProfile());
        const uint16_t mask = radioDiscoveryCadDetectedMask();
        char names[40] = {0};
        for (uint8_t i = 0; i < plan.count && i < 16; i++) {
            if (!(mask & (1U << i))) continue;
            const char *label = uiDiscoveryCandidateLabel(plan.candidates[i]);
            const size_t used = strlen(names);
            const size_t needed = (used == 0 ? 0 : 2) + strlen(label);
            if (used + needed >= sizeof(names) - 4) {
                strcat(names, "...");
                break;
            }
            if (used != 0) strcat(names, ", ");
            strcat(names, label);
        }
        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->print(names);
    }

    // The raw per-channel breakdown, restored as a single reference line
    // under the readable summary above rather than dropped — full detail
    // still lives in probe.csv, but the operator shouldn't have to pull
    // the card to see "how many timed out" right after a run.
    char stats[40];
    snprintf(stats, sizeof(stats), "hits %u  free %u  timeout %u  err %u",
             (unsigned)hits, (unsigned)free, (unsigned)timeouts, (unsigned)errors);
    uiTft->setTextColor(errors > 0 ? COL_BAD : COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 84);
    uiTft->print(stats);

    if (state == DiscoverySweepState::FAILED) {
        char value[10];
        snprintf(value, sizeof(value), "%d", radioLastError());
        statBlock(2, HEADER_H + 100, "radio error", value, COL_BAD);
    }
}

// Track + marker, not a fill bar — frequency is a *position* within a band,
// not a proportion of something used up. Defaults to 868-923MHz, the SX1262
// front end's actual tuned range (docs/DESIGN.md §1, not the full
// 902-928MHz US ISM band) for drawChannelPage()/drawSweepPage(); Cell's own
// card (Phase 11) passes its own, much narrower 869-894MHz band explicitly
// so the marker actually resolves movement within it instead of being lost
// in a ~25MHz sliver of the full front end's range.
void drawFreqBar(int16_t x, int16_t y, int16_t w, float freqMhz, float lo = 868.0f, float hi = 923.0f) {
    uiTft->drawFastHLine(x, y, w, COL_DIM);
    float frac = (freqMhz - lo) / (hi - lo);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t mx = x + (int16_t)((w - 3) * frac);
    uiTft->fillRect(mx, y - 3, 3, 7, COL_GOOD);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(x, y + 6);
    uiTft->print((int)lo);
    char hiBuf[8];
    snprintf(hiBuf, sizeof(hiBuf), "%d", (int)hi);
    uiTft->setCursor(x + w - (int16_t)strlen(hiBuf) * 6, y + 6);
    uiTft->print(hiBuf);
}

// Meter's own bar gauge (operator request, 2026-09-04) — unlike
// drawFreqBar()'s deliberate "position, not proportion" marker (frequency
// is a location within a band), signal strength genuinely IS a quantity —
// more power really is "more" — so a filled bar is the honest shape here,
// not a marker on a line. Neutral colour (COL_FG fill, COL_DIM border) on
// purpose: no strength-tier colour-coding (green=strong/red=weak) — no
// such convention exists anywhere else in this codebase, and a raw dBm
// number has no universal "good/bad" line without knowing SF/BW context,
// so inventing thresholds here would be exactly the kind of fabricated
// meaning this project is otherwise careful to avoid.
void drawMeterBar(int16_t x, int16_t y, int16_t w, int16_t h, float dbm, float lo, float hi) {
    uiTft->drawRect(x, y, w, h, COL_DIM);
    float frac = (dbm - lo) / (hi - lo);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t fillW = (int16_t)((w - 2) * frac);
    if (fillW > 0) uiTft->fillRect(x + 1, y + 1, fillW, h - 2, COL_FG);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(x, y + h + 3);
    uiTft->print((int)lo);
    char hiBuf[8];
    snprintf(hiBuf, sizeof(hiBuf), "%d", (int)hi);
    uiTft->setCursor(x + w - (int16_t)strlen(hiBuf) * 6, y + h + 3);
    uiTft->print(hiBuf);
}

// Waterfall's own frequency axis (operator request, 2026-09-04) — a plain
// labeled scale, not drawFreqBar()'s "current position" marker convention:
// Waterfall shows history across many rows, not one live scan, so a single
// "you are here" mark has no honest meaning here. Four unlabeled reference
// ticks at 20/40/60/80% (the "checkmarks for each band" ask) plus three
// labeled points (lo/center/hi), one decimal place to match drawFreqBar()'s
// own MHz precision elsewhere. Sized for the full plot content width
// (unlike drawFreqBar()'s calls on Sweep/Cell, which only ever use their
// page's narrower two-column layout).
//
// Waterfall rows don't record which Region (US/Global) was active when
// pushed (waterfall.h's WaterfallRow has no region field) — this labels the
// CURRENT live region's band, same "best current guess" drawSweepPage()'s
// own drawFreqBar() call already relies on for its one scan. Slightly wrong
// for older rows only if Region was toggled mid-history, an edge case, not
// the common path.
//
// No hline of its own (operator request, 2026-09-04: "can the bottom of
// the waterfall chart become the marker for the frequency?") — `y` here is
// the plot box's own bottom border (drawWaterfallPage() passes PLOT_Y +
// PLOT_H directly, no gap), not a separate line drawn a few px below it.
// Ticks hang down from that shared border instead of straddling a second
// line, closing what used to be a small but real double-line gap and
// giving that height back to the box (see PLOT_H's own comment).
void drawWaterfallFreqAxis(int16_t x, int16_t y, int16_t w, float lo, float hi) {
    static const float kTickFracs[] = {0.2f, 0.4f, 0.6f, 0.8f};
    for (float frac : kTickFracs) {
        uiTft->drawFastVLine(x + (int16_t)(w * frac), y + 1, 3, COL_DIM);
    }
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1f", (double)lo);
    uiTft->setCursor(x, y + 4);
    uiTft->print(buf);
    snprintf(buf, sizeof(buf), "%.1f", (double)hi);
    uiTft->setCursor(x + w - (int16_t)strlen(buf) * 6, y + 4);
    uiTft->print(buf);
    snprintf(buf, sizeof(buf), "%.1f", (double)((lo + hi) / 2.0f));
    uiTft->setCursor(x + w / 2 - (int16_t)strlen(buf) * 3, y + 4);
    uiTft->print(buf);
}

// One dim tick per peak bin, same height/vertical span as drawFreqBar()'s
// own position marker (a 3x7 rect centred on the line) so the two read as
// the same kind of mark — just thin (1px wide) since many bins can share
// one pixel column. A cheap occupancy sketch (radioEnergyPeakBinSet() is a
// 28-byte bitmask, not a per-bin RSSI history) showing *where* in the band
// activity clustered, not just how many bins hit. Deliberately not a real
// spectrum/waterfall (CLAUDE.md truthful-visualization rule): a stored
// peak is a threshold-filtered occupancy fact, not a signal-strength plot.
void drawSweepOccupancy(int16_t x, int16_t y, int16_t w, uint16_t totalBins) {
    for (uint16_t bin = 0; bin < totalBins; bin++) {
        if (!radioEnergyPeakBinSet(bin)) continue;
        float frac = (float)bin / (float)(totalBins > 1 ? totalBins - 1 : 1);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        const int16_t tx = x + (int16_t)((w - 1) * frac);
        uiTft->fillRect(tx, y - 3, 1, 7, COL_WARN);
    }
}

// Phase 9 Sweep result card, same layout shape as drawProbePage() above.
// Reuses drawFreqBar() above to show the current/last scanned bin as a
// position on the tuned band, same truthful-position convention CHANNEL's
// own frequency bar already uses — never a fill/progress bar. The
// disclaimer line stays on screen deliberately (reworded 2026-08-29,
// operator request, same meaning as the original "energy only, not
// LoRa"): docs/DESIGN.md's central Sweep rule is that a measured RSSI peak is
// never by itself evidence of LoRa traffic.
void drawSweepPage() {
    const EnergySweepState state = radioEnergySweepState();
    const uint16_t bin = radioEnergyBinIndex();
    const uint16_t total = radioEnergyBinCount();
    const uint16_t peaks = radioEnergyPeakCount();
    const bool repeating = radioEnergySweepRepeatIsActive();
    const EnergySweepBand band = energySweepBandForRegion(radioEnergySweepRegion());

    uiTft->setTextSize(2);
    uiTft->setCursor(2, HEADER_H + 8);
    if (state == EnergySweepState::IDLE) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("NO SWEEP YET");
        uiTft->setTextSize(1);
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print("S: sweep   R: repeat");
        return;
    }

    const bool running = state == EnergySweepState::RUNNING;
    // Same IDLE-after-hold reversion as drawProbePage() — the headline
    // word is the only thing that changes; bin/peak data below still
    // reflects the real last result. Repeat mode (R, operator request
    // 2026-08-29; moved off a Ctrl+S chord to its own key 2026-08-30 — see
    // keyboard.h) takes priority over all of that: back-to-back laps would
    // otherwise flicker SCANNING/COMPLETE every single one, which reads as
    // broken rather than as one continuous ambient scan.
    const bool holdExpired = !repeating && !running && sweepTerminalShownAt != 0 &&
                             millis() - sweepTerminalShownAt >= RESULT_HOLD_MS;
    if (repeating) {
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->print("REPEATING");
    } else if (holdExpired) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("IDLE");
    } else {
        const uint16_t colour = state == EnergySweepState::FAILED
                                    ? COL_BAD
                                    : (running || state == EnergySweepState::CANCELLED ? COL_WARN : COL_GOOD);
        uiTft->setTextColor(colour, COL_BG);
        if (running) {
            uiTft->print("SCANNING");
        } else if (state == EnergySweepState::COMPLETE) {
            uiTft->print("COMPLETE");
        } else if (state == EnergySweepState::CANCELLED) {
            uiTft->print("CANCELLED");
        } else {
            uiTft->print("FAILED");
        }
    }

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 31);
    if (running) {
        uiTft->print("watch paused  ");
    } else {
        uiTft->print("bins ");
    }
    uiTft->print(bin);
    uiTft->print('/');
    uiTft->print(total);
    if (!running) {
        uiTft->print("  away ");
        uiTft->print(radioEnergyLastAwayMs());
        uiTft->print("ms");
    }

    drawFreqBar(2, HEADER_H + 62, 108, energyBinFrequencyMhz(bin, band, ENERGY_SWEEP_DEFAULT_STEP),
               band.lo_mhz, band.hi_mhz);
    drawSweepOccupancy(2, HEADER_H + 62, 108, total);

    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 96);
    uiTft->print("listening to the noise");

    char value[10];
    snprintf(value, sizeof(value), "%u", (unsigned)peaks);
    statBlock(170, HEADER_H + 6, "peaks", value, peaks == 0 ? COL_DIM : COL_WARN);
    if (state == EnergySweepState::FAILED) {
        snprintf(value, sizeof(value), "%d", radioLastError());
        statBlock(170, HEADER_H + 34, "radio", value, COL_BAD);
    } else {
        // The single strongest peak this sweep — a more useful "what did
        // we find" callout than a bare count.
        const EnergyStrongestPeak strongest = radioEnergyStrongestPeak();
        if (strongest.valid) {
            char freqBuf[10];
            snprintf(freqBuf, sizeof(freqBuf), "%.1f", (double)strongest.freq_mhz);
            statBlock(170, HEADER_H + 34, "best MHz", freqBuf, COL_WARN);
            char rssiBuf[10];
            snprintf(rssiBuf, sizeof(rssiBuf), "%ddB", (int)(strongest.rssi_peak_dbm_x10 / 10));
            statBlock(170, HEADER_H + 62, "rssi", rssiBuf);
        } else {
            uiTft->setTextColor(COL_DIM, COL_BG);
            uiTft->setCursor(170, HEADER_H + 34);
            uiTft->print("none found");
        }
    }

    // Lap counter, bottom of the right column (operator request,
    // 2026-08-29) — separate from peaks/best-MHz/rssi above it, which
    // still describe the most recently finished lap, not the chain itself.
    if (repeating) {
        char lapValue[10];
        snprintf(lapValue, sizeof(lapValue), "%lu", (unsigned long)radioEnergySweepRepeatCount());
        statBlock(170, HEADER_H + 90, "lap", lapValue, COL_WARN);
    }
}

// Phase 11 Cell result card (2026-09-01), same layout shape as
// drawSweepPage() above — this is the same bounded-bin-sweep pattern, just
// scoped to cell_plan.h's 101-bin, 869-894MHz band instead of the full
// front end, and with no CAD/Pass-B step (cell_plan.h's file header: CAD
// never fires on a cellular carrier, so there is nothing to attempt).
// drawFreqBar() is called with Cell's own band bounds, not the default
// 868-923MHz, so the marker resolves position within the actual band being
// swept instead of a barely-visible sliver of the full front end. The
// disclaimer line stays on screen deliberately, same reasoning as Sweep's
// own "listening to the noise" line: docs/DESIGN.md §5a's central rule is
// that a cell-band RSSI reading is never presented as anything more than
// presence/strength — no cell ID, no decode, no tower location.
// FCC A/B block markers under the Cell frequency bar (drawFreqBar()
// above, called with Cell's own 869-894MHz band) — same thin-tick idiom
// as drawSweepOccupancy()'s peak marks, positionally aligned so a reader
// can see which block the current bin sits in. Grayscale only (COL_DIM/
// COL_FG): colour is already claimed elsewhere on this page (COL_GOOD =
// position marker, COL_WARN = repeat/caution, COL_BAD = error), and a
// regulatory block boundary isn't itself good/bad/urgent. cell_plan.h's
// CELL_BAND_BLOCKS is the FCC's own split (47 CFR § 22.905) — a letter
// label only, never a carrier name (see that file's citation comment).
void drawCellBandBlocks(int16_t x, int16_t y, int16_t w) {
    for (uint8_t i = 0; i < CELL_BAND_BLOCK_COUNT; i++) {
        const CellBandBlock &block = CELL_BAND_BLOCKS[i];
        const float loFrac = (block.lo_mhz - CELL_SWEEP_BAND_LO_MHZ) /
                              (CELL_SWEEP_BAND_HI_MHZ - CELL_SWEEP_BAND_LO_MHZ);
        const float hiFrac = (block.hi_mhz - CELL_SWEEP_BAND_LO_MHZ) /
                              (CELL_SWEEP_BAND_HI_MHZ - CELL_SWEEP_BAND_LO_MHZ);
        const int16_t sx = x + (int16_t)(w * loFrac);
        int16_t segW = x + (int16_t)(w * hiFrac) - sx;
        if (segW < 1) segW = 1;
        const uint16_t colour = (block.label == 'A') ? COL_DIM : COL_FG;
        uiTft->fillRect(sx, y, segW, 2, colour);
        // Only the two >=11MHz segments are wide enough to hold a glyph
        // without crowding; the two ~1.5-2.5MHz segments still get their
        // tick, just no letter.
        if (segW >= 8) {
            uiTft->setTextSize(1);
            uiTft->setTextColor(colour, COL_BG);
            uiTft->setCursor(sx, y + 4);
            uiTft->print(block.label);
        }
    }
}

void drawCellPage() {
    const CellSweepState state = radioCellSweepState();
    const uint16_t bin = radioCellBinIndex();
    const uint16_t total = radioCellBinCount();
    const bool repeating = radioCellSweepRepeatIsActive();

    uiTft->setTextSize(2);
    uiTft->setCursor(2, HEADER_H + 8);
    if (state == CellSweepState::IDLE) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("NO SCAN YET");
        uiTft->setTextSize(1);
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print("C: scan   R: repeat");
        return;
    }

    const bool running = state == CellSweepState::RUNNING;
    // Same IDLE-after-hold reversion as drawProbePage()/drawSweepPage() —
    // only the headline word changes; bin/signal data below still reflects
    // the real last result. Repeat mode takes priority over that, same
    // reasoning as drawSweepPage()'s own repeating check: back-to-back laps
    // would otherwise flicker SCANNING/COMPLETE every single one.
    const bool holdExpired = !repeating && !running && cellTerminalShownAt != 0 &&
                             millis() - cellTerminalShownAt >= RESULT_HOLD_MS;
    if (repeating) {
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->print("REPEATING");
    } else if (holdExpired) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("IDLE");
    } else {
        const uint16_t colour = state == CellSweepState::FAILED
                                    ? COL_BAD
                                    : (running || state == CellSweepState::CANCELLED ? COL_WARN : COL_GOOD);
        uiTft->setTextColor(colour, COL_BG);
        if (running) {
            uiTft->print("SCANNING");
        } else if (state == CellSweepState::COMPLETE) {
            uiTft->print("COMPLETE");
        } else if (state == CellSweepState::CANCELLED) {
            uiTft->print("CANCELLED");
        } else {
            uiTft->print("FAILED");
        }
    }

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 31);
    if (running) {
        uiTft->print("watch paused  ");
    } else {
        uiTft->print("bins ");
    }
    uiTft->print(bin);
    uiTft->print('/');
    uiTft->print(total);
    if (!running) {
        uiTft->print("  away ");
        uiTft->print(radioCellLastAwayMs());
        uiTft->print("ms");
    }

    drawFreqBar(2, HEADER_H + 62, 108, cellBinFrequencyMhz(bin), CELL_SWEEP_BAND_LO_MHZ,
               CELL_SWEEP_BAND_HI_MHZ);
    drawCellBandBlocks(2, HEADER_H + 80, 108);

    // Permanent honesty line, not a one-time disclaimer — this is the
    // feature's central rule (docs/DESIGN.md §5a), not incidental text.
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 96);
    uiTft->print("RSSI only - no decode");

    if (state == CellSweepState::FAILED) {
        char value[10];
        snprintf(value, sizeof(value), "%d", radioLastError());
        statBlock(170, HEADER_H + 6, "radio", value, COL_BAD);
    } else {
        // The single strongest reading this sweep — same "what did we find"
        // callout shape as Sweep's own strongest-peak block, just without a
        // peak *count* (Cell logs every bin, not threshold-filtered peaks —
        // cell_observation.h's file header).
        const CellStrongestSignal strongest = radioCellStrongestSignal();
        if (strongest.valid) {
            char freqBuf[10];
            snprintf(freqBuf, sizeof(freqBuf), "%.1f", (double)strongest.freq_mhz);
            statBlock(170, HEADER_H + 6, "best MHz", freqBuf, COL_WARN);
            char rssiBuf[10];
            snprintf(rssiBuf, sizeof(rssiBuf), "%ddB", (int)(strongest.rssi_peak_dbm_x10 / 10));
            statBlock(170, HEADER_H + 34, "rssi", rssiBuf);
        } else {
            uiTft->setTextColor(COL_DIM, COL_BG);
            uiTft->setCursor(170, HEADER_H + 6);
            uiTft->print("none found");
        }
    }

    // Lap counter, same bottom-right placement convention as Sweep's own
    // (drawSweepPage() above) — Cell's right column only has two stat
    // blocks (no "peaks" concept, cell_observation.h's file header), so its
    // free slot is +62 rather than Sweep's +90. Shown even on a FAILED lap
    // (the branch above no longer returns early) so a failed lap mid-chain
    // doesn't hide how many laps ran before it.
    if (repeating) {
        char lapValue[10];
        snprintf(lapValue, sizeof(lapValue), "%lu", (unsigned long)radioCellSweepRepeatCount());
        statBlock(170, HEADER_H + 62, "lap", lapValue, COL_WARN);
    }
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

// Read-only RF detail behind RADIO's counters — an on-device way to
// confirm a profile switch actually retuned the radio, rather than
// trusting the header text alone.
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

    // Right column — the radio-mode label (docs/BRAND.md's "Watch" for
    // HOME_LISTEN) is the only one of the three mode labels with anything
    // to name until Phases 8/9 add the other two radio states.
    statBlock(170, HEADER_H + 6, "mode", uiModeLabelWatch());
    char airtimeBuf[16];
    snprintf(airtimeBuf, sizeof(airtimeBuf), "~%lums", (unsigned long)estimateTimeOnAirMs(ch.sf, ch.bw_khz));
    statBlock(170, HEADER_H + 34, "airtime", airtimeBuf);
}

// 4 small bars instead of a dim "GP:12 GL:6 GA:2 BD:2" text line — same
// per-constellation counts, scannable at a glance instead of read digit by
// digit. Capped at 4 talkers: the right column is 70px wide (x=170..240)
// and 4 bars at a 16px pitch fit it with room to spare.
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

        // Right column: satellites USED — the old layout showed
        // sats-in-view before a fix but never the used count once a fix
        // landed.
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

    // Right column, under sats/qual (or under NO FIX in the no-fix
    // branch) — shared 170..240 zone with the other pages.
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

// Outline + proportional fill, same visual language as drawBattery() —
// turns "312k heap" into something scannable instead of a number to
// compare against 512 in your head. ~512KB is the ESP32-S3FN8's total SRAM
// with no PSRAM (docs/DESIGN.md §1).
//
// Fills with USAGE, not remaining free space: a bar that grows as the
// budget is consumed reads the same direction as the colour tiers above it
// (both escalate toward "full is bad").
void drawHeapBar(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t freeHeapK, uint16_t colour) {
    uiTft->drawRect(x, y, w, h, colour);
    const uint32_t usedK = (freeHeapK < HEAP_BUDGET_KB) ? (HEAP_BUDGET_KB - freeHeapK) : HEAP_BUDGET_KB;
    float frac = (float)usedK / (float)HEAP_BUDGET_KB;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int16_t fill = (int16_t)((w - 2) * frac);
    if (fill > 0) uiTft->fillRect(x + 1, y + 1, fill, h - 2, colour);
}

// SYSTEM: minutes up, heap (text + bar), a 2x2 stat grid (min heap,
// battery, SPI bus contention, WiFi state), keyboard/health/version footer.
// WiFi's old dedicated carousel page was merged in here (Phase 6): SSID
// moved to the WIFI_TOGGLE toast instead (ui_actions.cpp), this page now
// shows only ON/OFF + client count.
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
    // Col B: x=205, holding its own gap from col A ("3.98V" is the longest
    // value and about as far right as it still fits with margin).
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
// carried by inverting FG/BG on the whole row width, same convention
// drawBattery()/drawRadioPage() use for a single value. The label/value
// separator lives here, not baked into each label string as a trailing
// space — removes an easy, silent mistake in the table. scrollHint ('^'/
// 'v'/0) is drawMenuList()'s own "more rows this way" cue for the top/
// bottom visible row of a scrolled list — drawn here, in the row's own
// unused right margin, so it inherits that row's already-inverted colours
// for free when the top or bottom row happens to be the selected one.
void drawMenuRow(int16_t y, const char *rowLabel, const char *value, bool selected, char scrollHint = 0) {
    const uint16_t fg = selected ? COL_BG : COL_FG;
    const uint16_t bg = selected ? COL_FG : COL_BG;
    uiTft->fillRect(0, y - 3, uiTft->width(), 20, bg);
    uiTft->setTextSize(2);
    uiTft->setTextColor(fg, bg);
    uiTft->setCursor(4, y);
    uiTft->print(rowLabel);
    if (value != nullptr && value[0] != '\0') {
        uiTft->print(": ");
        uiTft->print(value);
    }
    if (scrollHint != 0) {
        uiTft->setCursor(uiTft->width() - 14, y);
        uiTft->print(scrollHint);
    }
}

// What an ACTION row's value column shows. Generic over every list in the
// menu (Profile's choices, System's toggles, Display's Idle-dim cycle) —
// MenuState/MenuItem are data-driven (ui_menu.h), so this stays one switch
// on MenuAction rather than one function per list. `buf` backs the four
// OPEN_* cases below that need to format a live number rather than return a
// fixed string literal — safe as a single shared static buffer because
// each call's result is fully consumed (printed by drawMenuRow(), from
// drawMenuList()'s single-threaded per-row loop) before the next call runs.
const char *menuEntryValue(MenuAction action) {
    static char buf[16];
    switch (action) {
        // Tools' own three rows (operator report, 2026-09-05: restoring the
        // live per-tool status these rows showed as their own carousel hub
        // page, lost when that page was folded into a plain menu GROUP).
        // Same state vocabulary and RESULT_HOLD_MS dim-IDLE reversion
        // drawProbePage()/drawSweepPage()/drawCellPage() themselves use, so
        // this row never advertises a result the real page has already
        // reverted past.
        case MenuAction::OPEN_PROBE: {
            const DiscoverySweepState s = radioDiscoverySweepState();
            const bool holdExpired = s != DiscoverySweepState::RUNNING && probeTerminalShownAt != 0 &&
                                     millis() - probeTerminalShownAt >= RESULT_HOLD_MS;
            if (s == DiscoverySweepState::RUNNING) return "SCANNING";
            if (holdExpired || s == DiscoverySweepState::IDLE) return "IDLE";
            if (s == DiscoverySweepState::COMPLETE) return "COMPLETE";
            if (s == DiscoverySweepState::CANCELLED) return "CANCELLED";
            return "FAILED";
        }
        case MenuAction::OPEN_SWEEP: {
            const EnergySweepState s = radioEnergySweepState();
            const bool holdExpired = s != EnergySweepState::RUNNING && sweepTerminalShownAt != 0 &&
                                     millis() - sweepTerminalShownAt >= RESULT_HOLD_MS;
            if (radioEnergySweepRepeatIsActive()) return "REPEAT";
            if (s == EnergySweepState::RUNNING) return "SCANNING";
            if (holdExpired || s == EnergySweepState::IDLE) return "IDLE";
            if (s == EnergySweepState::COMPLETE) return "COMPLETE";
            if (s == EnergySweepState::CANCELLED) return "CANCELLED";
            return "FAILED";
        }
        case MenuAction::OPEN_CELL: {
            const CellSweepState s = radioCellSweepState();
            const bool holdExpired = s != CellSweepState::RUNNING && cellTerminalShownAt != 0 &&
                                     millis() - cellTerminalShownAt >= RESULT_HOLD_MS;
            if (radioCellSweepRepeatIsActive()) return "REPEAT";
            if (s == CellSweepState::RUNNING) return "SCANNING";
            if (holdExpired || s == CellSweepState::IDLE) return "IDLE";
            if (s == CellSweepState::COMPLETE) return "COMPLETE";
            if (s == CellSweepState::CANCELLED) return "CANCELLED";
            return "FAILED";
        }
        // Analyze's own five rows, same restoration reasoning as Tools'
        // three above — identical value logic to the deleted
        // drawAnalyzePage(), just returning through `buf` instead of a
        // page-local array.
        case MenuAction::OPEN_METER: {
            CaptureHistory captures;
            CaptureSummary latest;
            if (analyzerCaptureHistorySnapshot(captures, pdMS_TO_TICKS(20)) &&
                captureHistoryEntryAt(captures, 0, latest)) {
                snprintf(buf, sizeof(buf), "%ddBm", (int)latest.rssi_dbm);
                return buf;
            }
            return "--";
        }
        case MenuAction::OPEN_WATERFALL: {
            const uint8_t rows = analyzerWaterfallRowCount(pdMS_TO_TICKS(20));
            if (rows == 0) return "--";
            snprintf(buf, sizeof(buf), "%u rows", (unsigned)rows);
            return buf;
        }
        case MenuAction::OPEN_SCOPE: {
            if (radioScopeAcquireIsActive()) return "CAPTURING";
            const bool holdExpired = scopeTerminalShownAt != 0 &&
                                     millis() - scopeTerminalShownAt >= RESULT_HOLD_MS;
            ScopeTrace trace;
            if (!holdExpired && radioScopeTraceSnapshot(trace, 0) && trace.count > 0) return "CAPTURED";
            return "IDLE";
        }
        case MenuAction::OPEN_CAPTURES: {
            CaptureHistory captures;
            const bool have = analyzerCaptureHistorySnapshot(captures, pdMS_TO_TICKS(20));
            snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(have ? captures.count : 0),
                     (unsigned)CAPTURE_HISTORY_MAX_ENTRIES);
            return buf;
        }
        case MenuAction::OPEN_NODES: {
            NodeRoster roster;
            uint8_t liveNodes = 0;
            if (analyzerNodeRosterSnapshot(roster, pdMS_TO_TICKS(20))) {
                for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
                    if (roster.entries[i].node_id != NODE_ROSTER_EMPTY_ID) liveNodes++;
                }
            }
            snprintf(buf, sizeof(buf), "%u/%u", (unsigned)liveNodes, (unsigned)NODE_ROSTER_MAX_ENTRIES);
            return buf;
        }
        case MenuAction::SELECT_MESHTASTIC:
            return radioActiveProfile() == MissionProfile::MESHTASTIC ? "ACTIVE" : "";
        case MenuAction::SELECT_MESHCORE:
            return radioActiveProfile() == MissionProfile::MESHCORE ? "ACTIVE" : "";
        case MenuAction::WIFI_TOGGLE: return wifiIsEnabled() ? "ON" : "OFF";
        case MenuAction::DEBUG_TOGGLE: return loggerDebugIsEnabled() ? "ON" : "OFF";
        case MenuAction::IDENTITY_CAPTURE_TOGGLE: return radioIdentityCaptureIsEnabled() ? "ON" : "OFF";
        case MenuAction::SD_RETRY: return loggerSdReady() ? "READY" : "RETRY";
        case MenuAction::SERIAL_CONTROL_TOGGLE: return serialControlIsEnabled() ? "ON" : "OFF";
        case MenuAction::TRACE_TOGGLE: return radioIsTracePaused() ? "STANDBY" : "ACTIVE";
        case MenuAction::PROBE_TOGGLE:
            return radioDiscoverySweepIsActive() ? "RUNNING" : "";
        case MenuAction::IDLE_TIMEOUT_CYCLE: return IDLE_TIMEOUT_OPTIONS[idleTimeoutIndex].label;
        case MenuAction::REGION_CYCLE: return regionLabel(radioEnergySweepRegion());
        case MenuAction::CAPTURE_WINDOW_CYCLE: {
            // Resolve the live ms back to its option label rather than
            // caching an index here — radio_task.cpp owns the value.
            const uint32_t ms = radioEnergySweepHomeListenMs();
            for (uint8_t i = 0; i < CAPTURE_WINDOW_OPTION_COUNT; i++) {
                if (CAPTURE_WINDOW_OPTIONS_MS[i] == ms) return CAPTURE_WINDOW_LABELS[i];
            }
            return "";
        }
        // BRIGHTNESS_UP/DOWN aren't ACTION rows, so they never reach here.
        default: return "";
    }
}

// Read-only mirror of whichever bounded action is currently running
// (operator request, 2026-09-05) — carousel slot 2. Replaces
// drawRadioPage()'s own STANDBY/Probe/repeat banner (dropped the same
// session once this existed to do the job properly, on operator request:
// "we can drop the activity state from the radio page now that activity
// has its own card"). Never starts or cancels anything itself, only
// reports state — same "no duplicate entry point" reasoning that keeps
// Probe/Sweep/Cell off the root menu applies in reverse here: this page
// has no SELECT/REPEAT handling of its own, so it can't become a second
// way to start a scan.
//
// Reuses drawFreqBar()/drawSweepOccupancy()/drawCellBandBlocks()/
// statBlock() and copies drawSweepPage()/drawCellPage()'s own RUNNING-
// state geometry verbatim (same x/y offsets) rather than inventing a
// second layout — same live numbers, same proven-not-to-collide
// positions, just trimmed to the running case only (this page never shows
// a terminal COMPLETE/CANCELLED/FAILED state; the idle branch below covers
// "nothing running" instead, once per tool, reusing menuEntryValue()'s own
// OPEN_* cases and drawMenuRow() — this is why the function lives here,
// after both, rather than up with the other page-draw functions).
void drawActivityPage() {
    if (radioDiscoverySweepIsActive()) {
        const uint8_t current = radioDiscoveryCandidateIndex();
        const uint8_t total = radioDiscoveryCandidateCount();
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("PROBE");
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->setCursor(2, HEADER_H + 31);
        uiTft->print("watch paused  ");
        uiTft->print(current);
        uiTft->print('/');
        uiTft->print(total);
        uiTft->print(" candidates");

        // Candidates land one at a time with nothing partial to report
        // meanwhile (drawProbePage()'s own comment: "nothing else to
        // summarize until a candidate lands somewhere") — a plain progress
        // bar is the one genuinely live thing left to draw.
        constexpr int16_t BAR_X = 2, BAR_Y = HEADER_H + 54, BAR_W = 200, BAR_H = 14;
        uiTft->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_WARN);
        if (total > 0) {
            const int16_t fill = (int16_t)((BAR_W - 2) * ((float)current / (float)total));
            if (fill > 0) uiTft->fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, COL_WARN);
        }
        return;
    }

    if (radioEnergySweepIsActive()) {
        const bool repeating = radioEnergySweepRepeatIsActive();
        const uint16_t bin = radioEnergyBinIndex();
        const uint16_t total = radioEnergyBinCount();
        const uint16_t peaks = radioEnergyPeakCount();
        const EnergySweepBand band = energySweepBandForRegion(radioEnergySweepRegion());

        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("SWEEP");
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->setCursor(2, HEADER_H + 31);
        // Repeat mode parks on the home channel between laps with RX armed
        // (v1.0.3) — genuinely still listening, unlike single-shot, which
        // fully occupies the radio the same way Probe/Cell do.
        uiTft->print(repeating ? "capturing on home  " : "watch paused  ");
        uiTft->print(bin);
        uiTft->print('/');
        uiTft->print(total);

        drawFreqBar(2, HEADER_H + 62, 108, energyBinFrequencyMhz(bin, band, ENERGY_SWEEP_DEFAULT_STEP),
                   band.lo_mhz, band.hi_mhz);
        drawSweepOccupancy(2, HEADER_H + 62, 108, total);

        char value[10];
        snprintf(value, sizeof(value), "%u", (unsigned)peaks);
        statBlock(170, HEADER_H + 6, "peaks", value, peaks == 0 ? COL_DIM : COL_WARN);
        const EnergyStrongestPeak strongest = radioEnergyStrongestPeak();
        if (strongest.valid) {
            char freqBuf[10];
            snprintf(freqBuf, sizeof(freqBuf), "%.1f", (double)strongest.freq_mhz);
            statBlock(170, HEADER_H + 34, "best MHz", freqBuf, COL_WARN);
        } else {
            uiTft->setTextColor(COL_DIM, COL_BG);
            uiTft->setCursor(170, HEADER_H + 34);
            uiTft->print("none found");
        }
        if (repeating) {
            char lapValue[10];
            snprintf(lapValue, sizeof(lapValue), "%lu", (unsigned long)radioEnergySweepRepeatCount());
            statBlock(170, HEADER_H + 90, "lap", lapValue, COL_WARN);
        }
        return;
    }

    if (radioCellSweepIsActive()) {
        const bool repeating = radioCellSweepRepeatIsActive();
        const uint16_t bin = radioCellBinIndex();
        const uint16_t total = radioCellBinCount();

        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("CELL");
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->setCursor(2, HEADER_H + 31);
        uiTft->print("watch paused  ");
        uiTft->print(bin);
        uiTft->print('/');
        uiTft->print(total);

        drawFreqBar(2, HEADER_H + 62, 108, cellBinFrequencyMhz(bin), CELL_SWEEP_BAND_LO_MHZ,
                   CELL_SWEEP_BAND_HI_MHZ);
        drawCellBandBlocks(2, HEADER_H + 80, 108);

        const CellStrongestSignal strongest = radioCellStrongestSignal();
        if (strongest.valid) {
            char freqBuf[10];
            snprintf(freqBuf, sizeof(freqBuf), "%.1f", (double)strongest.freq_mhz);
            statBlock(170, HEADER_H + 6, "best MHz", freqBuf, COL_WARN);
            char rssiBuf[10];
            snprintf(rssiBuf, sizeof(rssiBuf), "%ddB", (int)(strongest.rssi_peak_dbm_x10 / 10));
            statBlock(170, HEADER_H + 34, "rssi", rssiBuf);
        } else {
            uiTft->setTextColor(COL_DIM, COL_BG);
            uiTft->setCursor(170, HEADER_H + 6);
            uiTft->print("none found");
        }
        if (repeating) {
            char lapValue[10];
            snprintf(lapValue, sizeof(lapValue), "%lu", (unsigned long)radioCellSweepRepeatCount());
            statBlock(170, HEADER_H + 62, "lap", lapValue, COL_WARN);
        }
        return;
    }

    if (radioScopeAcquireIsActive()) {
        ScopeTrace trace;
        const bool have = radioScopeTraceSnapshot(trace, pdMS_TO_TICKS(20));
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("SCOPE");
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_FG, COL_BG);
        uiTft->setCursor(2, HEADER_H + 24);
        if (have) {
            char freqBuf[16];
            snprintf(freqBuf, sizeof(freqBuf), "%.3fMHz", (double)trace.tuned_freq_mhz);
            uiTft->print(freqBuf);
        }
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print("watch paused");
        return;
    }

    // Idle: nothing currently running. No hero line (operator feedback,
    // 2026-09-05: "get rid of the top IDLE line completely its redundant"
    // — most tools revert to their idle word within RESULT_HOLD_MS of
    // finishing, so a state word up top was uninformative most of the
    // time). Rows moved up to fill the space and show real last-result
    // numbers instead of a perishable state word — same source data each
    // tool's own dedicated card computes (radioDiscoveryCadDetectedCount()
    // etc.), not menuEntryValue()'s OPEN_* state words, which is what was
    // showing "IDLE" on all four rows most of the time in the first place.
    // Shows "IDLE" (plain, no summary) for a tool genuinely never fired
    // this boot (state == IDLE) — RUNNING is impossible here (the four
    // branches above already returned), so IDLE here can only mean that.
    //
    // NOTE: this also removes the only remaining on-screen indicator that
    // Trace itself is paused (drawRadioPage()'s old banner covered it,
    // then this page's own hero briefly did) — now visible only at
    // Menu > Tools > Trace. Flagged, not silently dropped; worth
    // revisiting if that turns out to matter in the field.
    {
        char value[16];
        if (radioDiscoverySweepState() == DiscoverySweepState::IDLE) {
            drawMenuRow(HEADER_H + 10, "Probe", "IDLE", false);
        } else {
            snprintf(value, sizeof(value), "%u/%u hits", (unsigned)radioDiscoveryCadDetectedCount(),
                     (unsigned)radioDiscoveryCandidateCount());
            drawMenuRow(HEADER_H + 10, "Probe", value, false);
        }
    }
    {
        char value[16];
        if (radioEnergySweepState() == EnergySweepState::IDLE) {
            drawMenuRow(HEADER_H + 34, "Sweep", "IDLE", false);
        } else {
            const EnergyStrongestPeak strongest = radioEnergyStrongestPeak();
            if (strongest.valid) {
                snprintf(value, sizeof(value), "%upk %.0fMHz", (unsigned)radioEnergyPeakCount(),
                         (double)strongest.freq_mhz);
            } else {
                snprintf(value, sizeof(value), "none found");
            }
            drawMenuRow(HEADER_H + 34, "Sweep", value, false);
        }
    }
    {
        char value[16];
        if (radioCellSweepState() == CellSweepState::IDLE) {
            drawMenuRow(HEADER_H + 58, "Cell", "IDLE", false);
        } else {
            const CellStrongestSignal strongest = radioCellStrongestSignal();
            if (strongest.valid) {
                snprintf(value, sizeof(value), "%.0fMHz %ddB", (double)strongest.freq_mhz,
                         (int)(strongest.rssi_peak_dbm_x10 / 10));
            } else {
                snprintf(value, sizeof(value), "none found");
            }
            drawMenuRow(HEADER_H + 58, "Cell", value, false);
        }
    }
    {
        char value[16];
        ScopeTrace trace;
        const bool have = radioScopeTraceSnapshot(trace, pdMS_TO_TICKS(20));
        int8_t latest = 0;
        if (!have || trace.count == 0 || !scopeTraceSampleAt(trace, 0, latest)) {
            drawMenuRow(HEADER_H + 82, "Scope", "IDLE", false);
        } else {
            // RSSI only, not frequency — Channel already shows the tuned
            // frequency, and "MHz -120dB" together overflowed a 240px row.
            snprintf(value, sizeof(value), "%ddBm", (int)latest);
            drawMenuRow(HEADER_H + 82, "Scope", value, false);
        }
    }
}

// A SLIDER row's live value, formatted for both the list row and the
// slider screen itself (drawMenuSlider() below) — one switch on
// sliderIncrease (unique per slider row) rather than one function per
// slider, same "data-driven over MenuAction" shape as menuEntryValue()
// above. Revisit if a third slider is added and this switch starts feeling
// cramped.
void sliderValueLabel(const MenuItem &item, char *out, size_t outSize) {
    switch (item.sliderIncrease) {
        case MenuAction::BRIGHTNESS_UP:
            snprintf(out, outSize, "%u%%", (unsigned)activeBrightnessPercent);
            break;
        case MenuAction::SWEEP_MARGIN_UP:
            snprintf(out, outSize, "%.1fdB", (double)radioEnergySweepMarginDbmX10() / 10.0);
            break;
        default:
            if (outSize > 0) out[0] = '\0';
            break;
    }
}

// Same switch, for the slider screen's fill-bar fraction (0..1) — kept
// separate from sliderValueLabel() rather than folded together since one
// formats text and the other computes geometry; same MenuAction-per-case
// shape either way.
float sliderFraction(const MenuItem &item) {
    switch (item.sliderIncrease) {
        case MenuAction::BRIGHTNESS_UP:
            return (float)(activeBrightnessPercent - BRIGHTNESS_MIN) / (float)(BRIGHTNESS_MAX - BRIGHTNESS_MIN);
        case MenuAction::SWEEP_MARGIN_UP:
            return (float)(radioEnergySweepMarginDbmX10() - ENERGY_SWEEP_MARGIN_MIN_DBM_X10) /
                   (float)(ENERGY_SWEEP_MARGIN_MAX_DBM_X10 - ENERGY_SWEEP_MARGIN_MIN_DBM_X10);
        default:
            return 0.0f;
    }
}

// How many rows fit at drawMenuRow()'s 24px pitch before the last one's
// bottom edge reaches the footer hint text — the same math
// SYSTEM_GROUP_ITEMS' own comment (ui_task.cpp) already worked out once
// (its 5th row collided with the footer at y=135 on a 135px-tall panel).
constexpr uint8_t MENU_LIST_VISIBLE_ROWS = 4;

// One list of rows, at whatever depth menu.currentList() currently is —
// root, System's list, or Display's nested list all draw through this
// same function; no depth-specific draw functions. Lists longer than
// MENU_LIST_VISIBLE_ROWS scroll: the window slides to keep the highlighted
// row always visible (Analyze is the first list to need this, five rows
// against a four-row ceiling — operator report, 2026-09-05), with a small
// '^'/'v' cue on the top/bottom visible row whenever more rows sit outside
// the window in that direction.
void drawMenuList() {
    const MenuItem *list = menu.currentList();
    const uint8_t count = menu.currentCount();
    const uint8_t sel = menu.currentIndex();

    uint8_t start = 0;
    if (count > MENU_LIST_VISIBLE_ROWS) {
        if (sel >= MENU_LIST_VISIBLE_ROWS) start = (uint8_t)(sel - MENU_LIST_VISIBLE_ROWS + 1);
        const uint8_t maxStart = (uint8_t)(count - MENU_LIST_VISIBLE_ROWS);
        if (start > maxStart) start = maxStart;
    }
    const uint8_t visible = count < MENU_LIST_VISIBLE_ROWS ? count : MENU_LIST_VISIBLE_ROWS;

    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t i = (uint8_t)(start + row);
        const MenuItem &item = list[i];
        const int16_t y = (int16_t)(HEADER_H + 10 + row * 24);
        const bool selected = sel == i;
        char scrollHint = 0;
        if (row == 0 && start > 0) scrollHint = '^';
        else if (row == (uint8_t)(visible - 1) && (uint8_t)(start + visible) < count) scrollHint = 'v';

        if (item.kind == ItemKind::SLIDER) {
            char valueBuf[12];
            sliderValueLabel(item, valueBuf, sizeof(valueBuf));
            drawMenuRow(y, item.label, valueBuf, selected, scrollHint);
        } else if (item.items == PROFILE_GROUP_ITEMS) {
            // "Profile: Meshtastic" — surfaces the live profile without
            // drilling into the group. Goes through the same label/value
            // path as every ACTION row so drawMenuRow's ": " separator
            // stays the one place that decides the shape, rather than this
            // branch building its own copy of it.
            drawMenuRow(y, item.label, uiProfileLabel(radioActiveProfile()), selected, scrollHint);
        } else if (item.kind == ItemKind::GROUP) {
            drawMenuRow(y, item.label, nullptr, selected, scrollHint);
        } else {
            drawMenuRow(y, item.label, menuEntryValue(item.action), selected, scrollHint);
        }
    }

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, uiTft->height() - 9);
    uiTft->print(",/. move   Enter select   ` back");
}

// Slider screen — generic over whichever SLIDER row is currently open
// (Brightness or Margin, ui_menu.h's MenuItem). Large live readout plus a
// filled-bar track, same outline+fill visual language as
// drawHeapBar()/drawFreqBar() rather than a third bar style.
void drawMenuSlider() {
    const MenuItem &item = menu.currentItem();

    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 10);
    char valueBuf[12];
    sliderValueLabel(item, valueBuf, sizeof(valueBuf));
    uiTft->print(valueBuf);

    constexpr int16_t BAR_X = 2, BAR_Y = HEADER_H + 40, BAR_W = 200, BAR_H = 14;
    uiTft->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_GOOD);
    const float frac = sliderFraction(item);
    const int16_t fill = (int16_t)((BAR_W - 2) * frac);
    if (fill > 0) uiTft->fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, COL_GOOD);

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, uiTft->height() - 9);
    // Enter now leaves the slider the same way ` (BACK) does (ui_menu.h's
    // handleSlider(), 2026-08-29) — hint text updated so it doesn't go
    // silently out of date the moment a real, working key isn't mentioned.
    uiTft->print(",/. adjust   Enter/` back");
}

// --- Field Analyzer (Phase 10) --------------------------------------------
// Presentation layer over real Watch/Probe/Sweep/Cell observations — none of
// these five views ever polls or reconfigures the radio (docs/research/
// LoRaTrace-Phases-7-10-Design.md §8.1), except Scope, whose own capture is
// requested from ui_task.cpp's maybeStartScopeAcquire()/SCOPE_TOGGLE, not
// from here.

// Meter: "packet RSSI, selected-bin RSSI, or current scope RSSI with its
// source identified... show measurement age" (§8.2) — prefers a live/recent
// Scope sample when Scope is what actually produced the newest number,
// otherwise the most recent real detection's RSSI. Never both at once and
// never unlabeled, so a frozen reading can't be mistaken for continuous
// sampling.
void drawMeterPage() {
    ScopeTrace trace;
    const bool haveTrace = radioScopeTraceSnapshot(trace, 0) && trace.count > 0;
    int8_t scopeSample = 0;
    const bool haveScopeSample = haveTrace && scopeTraceSampleAt(trace, 0, scopeSample);

    CaptureHistory captures;
    CaptureSummary latestCapture;
    const bool haveCapture = analyzerCaptureHistorySnapshot(captures, pdMS_TO_TICKS(50)) &&
                             captureHistoryEntryAt(captures, 0, latestCapture);

    // Scope wins when it's actively running, or when nothing else exists,
    // or when its own capture is newer than the latest packet — "current
    // scope RSSI", not a stale one left over from an earlier visit.
    const bool useScope = haveScopeSample &&
        (radioScopeAcquireIsActive() || !haveCapture || trace.start_millis >= latestCapture.rx_millis);

    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 8);

    if (useScope) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d dBm", (int)scopeSample);
        uiTft->print(buf);
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 30);
        char detail[32];
        snprintf(detail, sizeof(detail), "scope @ %.3fMHz", (double)trace.tuned_freq_mhz);
        uiTft->print(detail);
        // No SNR line here (operator request, 2026-09-04, mocked up first
        // in docs/research/analyzer-preview.html): Scope samples raw RSSI
        // at one tuned frequency without demodulating anything
        // (drawScopePage()'s own "never a spectrum, no decode" design), so
        // it genuinely has no SNR to report — honest omission, not an
        // inconsistency, same reasoning as Waterfall's "no fabricated
        // vertical texture" rule. Same reason the right-column SF/BW/CR
        // block below is watch-only too.
        uiTft->setCursor(2, HEADER_H + 54);
        if (radioScopeAcquireIsActive()) {
            uiTft->setTextColor(COL_WARN, COL_BG);
            uiTft->print("live - watch paused");
        } else {
            char ageBuf[24];
            snprintf(ageBuf, sizeof(ageBuf), "%lus ago", (unsigned long)((millis() - trace.start_millis) / 1000));
            uiTft->print(ageBuf);
        }
        // Range widened -30 -> 0 (operator feedback, 2026-09-04: real
        // readings from a close repeater hit -16dBm, clipping flat
        // against the old ceiling — a real defect, since a clipped bar
        // can't distinguish -16dBm from -5dBm). Deliberately not shared
        // with drawScopePage()'s own DISPLAY_LO/DISPLAY_HI (-120/-30,
        // unchanged) — that page's ceiling exists so trace heights stay
        // visually comparable across captures, a different reason than
        // Meter's own "show the SX1262's real RSSI range without
        // clipping" here.
        constexpr float DISPLAY_LO = -120.0f, DISPLAY_HI = 0.0f;
        drawMeterBar(2, HEADER_H + 72, 232, 12, (float)scopeSample, DISPLAY_LO, DISPLAY_HI);
    } else if (haveCapture) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.0f dBm", (double)latestCapture.rssi_dbm);
        uiTft->print(buf);
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 30);
        char detail[32];
        snprintf(detail, sizeof(detail), "watch @ %.3fMHz", (double)latestCapture.freq_mhz);
        uiTft->print(detail);
        // SNR (operator request, 2026-09-04): real data this page already
        // had access to and never showed — capture_history.h's
        // CaptureSummary carries snr_db for every real decoded packet.
        char snrBuf[16];
        snprintf(snrBuf, sizeof(snrBuf), "SNR: %+.1fdB", (double)latestCapture.snr_db);
        uiTft->setCursor(2, HEADER_H + 42);
        uiTft->print(snrBuf);
        uiTft->setCursor(2, HEADER_H + 54);
        char ageBuf[24];
        snprintf(ageBuf, sizeof(ageBuf), "%lus ago", (unsigned long)((millis() - latestCapture.rx_millis) / 1000));
        uiTft->print(ageBuf);
        // Right column ("what can we use the dead space on the right
        // for?", operator request, 2026-09-04) — the channel params this
        // exact packet was decoded with, also real CaptureSummary data
        // and also never shown. Right-aligned, vertically matched to the
        // freq/SNR/age rows it sits beside.
        char rightBuf[16];
        snprintf(rightBuf, sizeof(rightBuf), "SF%u", (unsigned)latestCapture.sf);
        uiTft->setCursor(232 - (int16_t)strlen(rightBuf) * 6, HEADER_H + 30);
        uiTft->print(rightBuf);
        snprintf(rightBuf, sizeof(rightBuf), "%.1fkHz", (double)latestCapture.bw_khz_x10 / 10.0);
        uiTft->setCursor(232 - (int16_t)strlen(rightBuf) * 6, HEADER_H + 42);
        uiTft->print(rightBuf);
        snprintf(rightBuf, sizeof(rightBuf), "CR4/%u", (unsigned)latestCapture.cr_denom);
        uiTft->setCursor(232 - (int16_t)strlen(rightBuf) * 6, HEADER_H + 54);
        uiTft->print(rightBuf);
        // Range widened -30 -> 0 (operator feedback, 2026-09-04: real
        // readings from a close repeater hit -16dBm, clipping flat
        // against the old ceiling — a real defect, since a clipped bar
        // can't distinguish -16dBm from -5dBm). Deliberately not shared
        // with drawScopePage()'s own DISPLAY_LO/DISPLAY_HI (-120/-30,
        // unchanged) — that page's ceiling exists so trace heights stay
        // visually comparable across captures, a different reason than
        // Meter's own "show the SX1262's real RSSI range without
        // clipping" here.
        constexpr float DISPLAY_LO = -120.0f, DISPLAY_HI = 0.0f;
        drawMeterBar(2, HEADER_H + 72, 232, 12, latestCapture.rssi_dbm, DISPLAY_LO, DISPLAY_HI);
    } else {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("NO MEASUREMENT");
    }
}

// Aggregates one already-quantized WaterfallRow down to plot columns,
// max-arbitrated (a strong single bin must not be diluted by quieter
// neighbors sharing its column) — same reasoning as waterfall.h's own
// int16-domain waterfallAggregateRow(), just operating on the byte-encoded
// storage a WaterfallRow already holds instead of raw dBm.
void waterfallRowToColumns(const WaterfallRow &row, uint8_t *outColumns, uint16_t columnCount) {
    for (uint16_t c = 0; c < columnCount; c++) outColumns[c] = WATERFALL_NO_DATA;
    for (uint16_t b = 0; b < row.bin_count; b++) {
        if (row.bins[b] == WATERFALL_NO_DATA) continue;
        const uint16_t col = waterfallColumnForBin(b, row.bin_count, columnCount);
        if (outColumns[col] == WATERFALL_NO_DATA || row.bins[b] > outColumns[col]) {
            outColumns[col] = row.bins[b];
        }
    }
}

// Waterfall: one row per completed Sweep, newest at top, x = frequency bin
// mapped to a plot column. Energy bins are occupancy only
// (analyzer_state.cpp's own comment on analyzerNoteSweepComplete() explains
// why) — quiet bins and no-data bins both render as background, so this
// reads as a scrolling history of drawSweepOccupancy()'s existing tick
// marks rather than a genuine RSSI-graded heatmap. Real Phase 9 sweep rows
// only (§8.6: "no fabricated vertical texture").
//
// Two colours, two different claims, deliberately never blended:
//   COL_WARN yellow — Pass A measured energy over the margin in this bin.
//   COL_GOOD green  — a real packet was demodulated and CRC-checked on the
//                     home channel during this row's listen window (v1.0.3).
// Green is the stronger fact and frequently appears without yellow: Pass
// A's per-bin glance is milliseconds against a 142-490ms packet, so it
// misses traffic the receiver then decodes cleanly. Before v1.0.3 this page
// could read a flat "QUIET" while packets were actively being captured —
// that gap is what the green channel exists to close.
//
// Laid out to match drawScope()'s shape (operator request, 2026-09-03: "we
// should model this for some of these other pages"): a headline callout,
// one compact metadata line, then a bordered fixed-height box — instead of
// the plot growing edge-to-edge and needing its own bottom-anchored caption
// that collided with the footer (same bug this rewrite also fixes).
void drawWaterfallPage() {
    // Content-column bounds, declared up front: needed both by the top-row
    // repeat badge below and the plot box further down.
    constexpr int16_t PLOT_X = 2;
    constexpr uint16_t PLOT_W = 232;

    // Row-at-a-time on purpose (analyzer_state.h's own comment): the full
    // WaterfallHistory is ~5.5KB, far too large to ever hold as a stack
    // local on ui_task's 4096B stack — an earlier version of this function
    // did exactly that and crashed ui_task on real hardware (2026-09-03).
    // One WaterfallRow (~232B) plus the column buffer below is a normal-
    // sized set of locals.
    const uint8_t rowCount = analyzerWaterfallRowCount(pdMS_TO_TICKS(50));
    if (rowCount == 0) {
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("NO SWEEPS YET");
        uiTft->setTextSize(1);
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print("Enter: start repeat Sweep");
        return;
    }

    // First pass: total hit-bin count across every stored row, not just
    // whatever fits in the box below — the same "what did we find" callout
    // shape drawSweepPage()/drawCellPage() already use (peaks / strongest
    // signal), for a consistent headline across all three pages.
    uint32_t totalHits = 0;
    uint32_t totalCaptures = 0;
    uint16_t latestBinCount = 0;
    WaterfallRow row;
    for (uint8_t r = 0; r < rowCount; r++) {
        if (!analyzerWaterfallRowSnapshot(r, row, pdMS_TO_TICKS(50))) break;
        if (r == 0) latestBinCount = row.bin_count;
        for (uint16_t b = 0; b < row.bin_count; b++) {
            if (row.bins[b] >= 200) totalHits++;
        }
        totalCaptures += row.capture_count;
    }

    uiTft->setTextSize(2);
    uiTft->setCursor(2, HEADER_H + 6);
    if (totalHits > 0) {
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->print(totalHits);
        uiTft->print(totalHits == 1 ? " HIT" : " HITS");
    } else if (totalCaptures > 0) {
        // Energy found nothing but packets were still decoded on the home
        // channel — the exact case that used to read a flat "QUIET" while
        // real traffic was being captured (docs/STATUS.md's dwell-timing
        // entry). Saying QUIET here would be false, so the capture count
        // becomes the headline in its own colour instead.
        uiTft->setTextColor(COL_GOOD, COL_BG);
        uiTft->print(totalCaptures);
        uiTft->print(totalCaptures == 1 ? " PKT" : " PKTS");
    } else {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("QUIET");
    }

    // Repeat-status badge, top row, right-aligned opposite the hit counter
    // (operator request, 2026-09-04 — first landed on the metadata line
    // below, moved up here same day): Waterfall is Sweep's own history
    // view, so an operator watching it needs to know without leaving the
    // page whether it's actively being fed right now — same "measure
    // without extracting energy.csv first" reasoning Serial Control's own
    // STATUS fields already follow elsewhere in this project. Enter
    // toggles it; see WATERFALL_SWEEP_REPEAT_TOGGLE. Size 1, not size 2
    // like the hit counter — a secondary status badge, not a second
    // headline competing for the same weight.
    const bool repeating = radioEnergySweepRepeatIsActive();
    if (repeating) {
        static const char kScanning[] = "SCANNING";
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(PLOT_X + PLOT_W - (int16_t)(sizeof(kScanning) - 1) * 6, HEADER_H + 6);
        uiTft->print(kScanning);
    }

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 24);
    char meta[48];
    // Captures get named here rather than left to the green marks alone —
    // the legend is what makes the two colours readable as two different
    // claims instead of one gradient.
    if (totalCaptures > 0) {
        snprintf(meta, sizeof(meta), "%u rows  %u bins  %lu pkt", (unsigned)rowCount,
                 (unsigned)latestBinCount, (unsigned long)totalCaptures);
    } else {
        snprintf(meta, sizeof(meta), "%u rows, newest first  %u bins", (unsigned)rowCount,
                 (unsigned)latestBinCount);
    }
    uiTft->print(meta);

    // Fixed-height bordered box, same visual language as drawScope()'s own
    // plot rect — its height (not "grow until the footer") is what keeps
    // this page's layout stable and collision-free regardless of history
    // depth. Top edge moved up from HEADER_H+44 to HEADER_H+34 (operator
    // request, 2026-09-04): the meta line above ends around HEADER_H+32,
    // so the box used to leave ~10px of dead space before it started —
    // closed that gap and gave the reclaimed height straight to PLOT_H
    // (45 -> 55) instead of just moving empty space around. Then the
    // frequency axis below merged into this box's own bottom border,
    // same day (see drawWaterfallFreqAxis()'s own comment) — that removed
    // both the axis's separate hline and the gap before it, reclaiming
    // another ~5px, handed to PLOT_H again (55 -> 60, ~4 more visible
    // rows total than the box originally shipped with). Axis footprint
    // below the box is now ~12px, leaving a real ~7px margin before
    // drawFooterStatus()'s text — tighter than before this merge, but
    // nowhere near the ~3px that caused this page's real 2026-09-03
    // footer collision.
    constexpr int16_t PLOT_Y = HEADER_H + 34;
    constexpr int16_t PLOT_H = 60;
    uiTft->drawRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, COL_DIM);

    constexpr int16_t ROW_H = 4;
    constexpr int16_t ROWS_BOTTOM = PLOT_Y + PLOT_H - 2;
    uint8_t columns[PLOT_W];
    for (uint8_t r = 0; r < rowCount; r++) {
        const int16_t y = PLOT_Y + 2 + (int16_t)r * ROW_H;
        if (y + ROW_H > ROWS_BOTTOM) break;
        if (!analyzerWaterfallRowSnapshot(r, row, pdMS_TO_TICKS(50))) break;

        waterfallRowToColumns(row, columns, PLOT_W);
        for (uint16_t c = 0; c < PLOT_W; c++) {
            if (columns[c] == WATERFALL_NO_DATA || columns[c] <= 1) continue; // quiet/no-data: background
            const uint16_t colour = columns[c] >= 200 ? COL_WARN : COL_DIM;
            uiTft->drawFastVLine(PLOT_X + (int16_t)c, y, ROW_H - 1, colour);
        }

        // Packets decoded on the home channel during this row's listen
        // window, drawn last so they win the pixel over an energy tick at
        // the same column. Two different facts share this plot and must
        // stay visually distinct (CLAUDE.md's truthful-visualization rule):
        // COL_WARN yellow = "Pass A measured energy over the margin here",
        // COL_GOOD green = "a real packet was demodulated and CRC-checked
        // here" — the stronger claim, and one Pass A frequently misses
        // entirely because its per-bin glance is milliseconds against a
        // 142-490ms packet. A green mark with no yellow under it is the
        // normal, expected case, not a contradiction.
        if (row.capture_bin != WATERFALL_NO_CAPTURE_BIN && row.capture_count > 0) {
            const uint16_t col = waterfallColumnForBin(row.capture_bin, row.bin_count, PLOT_W);
            uiTft->drawFastVLine(PLOT_X + (int16_t)col, y, ROW_H - 1, COL_GOOD);
        }
    }
    // Frequency axis below the box (operator request, 2026-09-04) — the
    // box's shrunk, fixed height (above) is what guarantees this stays
    // clear of the footer, not a "grow until it collides" layout the way
    // an earlier revision's own bottom caption did (height-9, colliding
    // with drawFooterStatus()'s profile text at height-10 on real
    // hardware, 2026-09-03 — caught in docs/research/analyzer-preview.html
    // before a second flash cycle, same tool that caught this addition's
    // own first, too-tight vertical-margin draft before it ever reached
    // real hardware).
    const EnergySweepBand band = energySweepBandForRegion(radioEnergySweepRegion());
    drawWaterfallFreqAxis(PLOT_X, PLOT_Y + PLOT_H, PLOT_W, band.lo_mhz, band.hi_mhz);
}

// Scope: x = time, y = RSSI, one fixed tuned frequency — never a spectrum
// (§8.2). ui_task.cpp requests the actual SCOPE_ACQUIRE; this only ever
// renders whatever ScopeTrace it's handed.
void drawScopePage() {
    ScopeTrace trace;
    const bool have = radioScopeTraceSnapshot(trace, pdMS_TO_TICKS(50));
    const bool running = radioScopeAcquireIsActive();
    const bool holdExpired = !running && scopeTerminalShownAt != 0 &&
                             millis() - scopeTerminalShownAt >= RESULT_HOLD_MS;

    uiTft->setTextSize(2);
    uiTft->setCursor(2, HEADER_H + 6);
    if (!have || trace.count == 0) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("NO SCOPE YET");
        uiTft->setTextSize(1);
        uiTft->setCursor(2, HEADER_H + 30);
        uiTft->print(running ? "watch paused" : "Enter to capture");
        return;
    }

    if (running) {
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->print("CAPTURING");
    } else if (holdExpired) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->print("IDLE");
    } else {
        uiTft->setTextColor(COL_GOOD, COL_BG);
        uiTft->print("CAPTURED");
    }

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 24);
    char freqBuf[28];
    snprintf(freqBuf, sizeof(freqBuf), "%.3fMHz  %ums/sample", (double)trace.tuned_freq_mhz,
             (unsigned)trace.sample_interval_ms);
    uiTft->print(freqBuf);
    if (running) {
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print("watch paused");
    }

    constexpr int16_t PLOT_X = 2, PLOT_Y = HEADER_H + 48, PLOT_W = 232, PLOT_H = 50;
    uiTft->drawRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, COL_DIM);

    // Fixed display range, not a per-trace auto-scale: a generous envelope
    // around any real SX1262 reading, chosen so two different captures'
    // trace heights stay visually comparable rather than each rescaling to
    // fill the box regardless of actual signal strength.
    constexpr int8_t DISPLAY_LO = -120, DISPLAY_HI = -30;
    int16_t prevX = -1, prevY = 0;
    for (uint16_t i = 0; i < trace.count; i++) {
        // Chronological left-to-right: i=0 is the oldest still-held sample
        // (recency index count-1), i=count-1 is the newest.
        int8_t value;
        if (!scopeTraceSampleAt(trace, (uint16_t)(trace.count - 1 - i), value)) continue;
        int16_t clamped = value;
        if (clamped < DISPLAY_LO) clamped = DISPLAY_LO;
        if (clamped > DISPLAY_HI) clamped = DISPLAY_HI;
        const uint16_t denom = trace.count > 1 ? (uint16_t)(trace.count - 1) : 1;
        const int16_t x = PLOT_X + (int16_t)((uint32_t)i * (PLOT_W - 1) / denom);
        const float frac = (float)(clamped - DISPLAY_LO) / (float)(DISPLAY_HI - DISPLAY_LO);
        const int16_t y = PLOT_Y + PLOT_H - 1 - (int16_t)(frac * (PLOT_H - 2));
        if (prevX >= 0) uiTft->drawLine(prevX, prevY, x, y, COL_GOOD);
        prevX = x;
        prevY = y;
    }

    uiTft->setTextColor(COL_DIM, COL_BG);
    char hiBuf[8];
    snprintf(hiBuf, sizeof(hiBuf), "%d", (int)DISPLAY_HI);
    uiTft->setCursor(PLOT_X, PLOT_Y - 8);
    uiTft->print(hiBuf);
    char loBuf[8];
    snprintf(loBuf, sizeof(loBuf), "%d", (int)DISPLAY_LO);
    uiTft->setCursor(PLOT_X, PLOT_Y + PLOT_H + 1);
    uiTft->print(loBuf);
}

// Recent Captures: "time, profile, frequency, SF/BW/CR, RSSI/SNR, length,
// safe cleartext header IDs. No payload hex, plaintext, or key handling"
// (§8.2) — CaptureSummary (capture_history.h) already enforces the "no
// payload" half at the data-structure level, so this only ever formats
// fields it's structurally incapable of leaking past.
void drawCapturesPage() {
    CaptureHistory history;
    const bool have = analyzerCaptureHistorySnapshot(history, pdMS_TO_TICKS(50));
    if (!have || history.count == 0) {
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("NO CAPTURES YET");
        return;
    }

    constexpr int16_t ROW_H = 13;
    constexpr int16_t FOOTER_Y = 126;
    uiTft->setTextSize(1);
    for (uint8_t i = 0; i < history.count; i++) {
        CaptureSummary summary;
        if (!captureHistoryEntryAt(history, i, summary)) break;
        const int16_t y = HEADER_H + 3 + (int16_t)i * ROW_H;
        if (y + ROW_H > FOOTER_Y) break;

        char line[48];
        const uint32_t ageS = (millis() - summary.rx_millis) / 1000;
        if (summary.node_id != 0) {
            snprintf(line, sizeof(line), "%s !%08lx %.1fMHz %ddBm %lus",
                     missionProfileName(summary.profile), (unsigned long)summary.node_id,
                     (double)summary.freq_mhz, (int)summary.rssi_dbm, (unsigned long)ageS);
        } else {
            snprintf(line, sizeof(line), "%s %.1fMHz %ddBm %lus", missionProfileName(summary.profile),
                     (double)summary.freq_mhz, (int)summary.rssi_dbm, (unsigned long)ageS);
        }
        uiTft->setTextColor(summary.off_grid ? COL_WARN : COL_FG, COL_BG);
        uiTft->setCursor(2, y);
        uiTft->print(line);
    }
}

// Passive Nodes: "cleartext node ID, last seen, packet count, best/latest
// RSSI and SNR, hop metadata if already available. Fixed roster only" (§8.2)
// — NodeRosterEntry (node_roster.h) has no field for a name, chat text, or
// position, so there is nothing here capable of showing them either.
// Recency-sorted for display only; the roster itself has no order beyond
// slot index (its own LRU eviction doesn't need one).
void drawNodesPage() {
    NodeRoster roster;
    if (!analyzerNodeRosterSnapshot(roster, pdMS_TO_TICKS(50))) {
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("NO NODES YET");
        return;
    }

    uint8_t order[NODE_ROSTER_MAX_ENTRIES];
    uint8_t liveCount = 0;
    for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        if (roster.entries[i].node_id != NODE_ROSTER_EMPTY_ID) order[liveCount++] = i;
    }
    if (liveCount == 0) {
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("NO NODES YET");
        return;
    }
    // Selection sort, newest-seen first — at most 24 entries, cheap enough
    // for a once-a-second redraw.
    for (uint8_t i = 0; i < liveCount; i++) {
        uint8_t best = i;
        for (uint8_t j = (uint8_t)(i + 1); j < liveCount; j++) {
            if (roster.entries[order[j]].last_seen_millis > roster.entries[order[best]].last_seen_millis) {
                best = j;
            }
        }
        if (best != i) {
            const uint8_t tmp = order[i];
            order[i] = order[best];
            order[best] = tmp;
        }
    }

    constexpr int16_t ROW_H = 13;
    constexpr int16_t FOOTER_Y = 126;
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_FG, COL_BG);
    for (uint8_t i = 0; i < liveCount; i++) {
        const int16_t y = HEADER_H + 3 + (int16_t)i * ROW_H;
        if (y + ROW_H > FOOTER_Y) break;
        const NodeRosterEntry &e = roster.entries[order[i]];
        const uint32_t ageS = (millis() - e.last_seen_millis) / 1000;
        char line[48];
        snprintf(line, sizeof(line), "%s !%08lx x%lu %ddBm %lus", missionProfileName(e.profile),
                 (unsigned long)e.node_id, (unsigned long)e.packet_count, (int)e.latest_rssi_dbm,
                 (unsigned long)ageS);
        uiTft->setCursor(2, y);
        uiTft->print(line);
    }
}

// Toast overlay — flush-bottom band that slides up from off-panel on show
// and carries a shrinking countdown bar along its own bottom edge. Both
// are just per-frame rectangle geometry (no alpha blending needed), driven
// by the bounded fast-redraw burst in ui_task.cpp while toastActive().
// Drawn last, on top of whatever page/menu is showing.
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
        // Breadcrumb, e.g. "MENU > System > Display" — full ancestor chain
        // since a GROUP can itself open another GROUP, plus the slider's
        // own label if one is open. Collected as whole segments (not a
        // flat string) so a too-long chain can drop entire segments from
        // the FRONT (oldest ancestor first, prefixed with "...") instead
        // of slicing into the middle of a label — the current/deepest
        // segment always survives intact, never a broken word fragment.
        const char *segments[MenuState::MAX_DEPTH + 1];
        uint8_t segmentCount = 0;
        constexpr uint8_t MAX_SEGMENTS = (uint8_t)(sizeof(segments) / sizeof(segments[0]));
        for (uint8_t i = 0; i < menu.breadcrumbCount() && segmentCount < MAX_SEGMENTS; i++) {
            segments[segmentCount++] = menu.breadcrumbLabel(i);
        }
        if (menu.inSlider() && segmentCount < MAX_SEGMENTS) {
            segments[segmentCount++] = menu.currentItem().label;
        }

        uiTft->setTextColor(COL_DIM, COL_BG);
        constexpr size_t MENU_LABEL_CHARS = 4; // strlen("MENU")
        uint8_t start = 0;
        for (;;) {
            size_t total = MENU_LABEL_CHARS + (start > 0 ? 3 : 0); // leading "..."
            for (uint8_t i = start; i < segmentCount; i++) total += 3 + strlen(segments[i]); // " > label"
            if (total <= HEADER_BREADCRUMB_MAX_CHARS || start >= segmentCount) break;
            start++;
        }
        if (start > 0) uiTft->print("...");
        for (uint8_t i = start; i < segmentCount; i++) {
            uiTft->print(" > ");
            uiTft->print(segments[i]);
        }
    } else {
        // Page name only — profile and page position live in the footer
        // status line (drawFooterStatus()) so this text never crowds the
        // status-dot cluster or the battery reading.
        uiTft->print(pageName(page));
    }

    drawBattery();

    // Status dot cluster: GPS fix state, heap health, RX activity —
    // always visible from any page instead of only their own.
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
            case UiPage::ACTIVITY: drawActivityPage(); break;
            case UiPage::CHANNEL: drawChannelPage(); break;
            case UiPage::GPS: drawGpsPage(); break;
            case UiPage::SYSTEM: drawSystemPage(); break;
            case UiPage::PROBE: drawProbePage(); break;
            case UiPage::SWEEP: drawSweepPage(); break;
            case UiPage::CELL: drawCellPage(); break;
            case UiPage::METER: drawMeterPage(); break;
            case UiPage::WATERFALL: drawWaterfallPage(); break;
            case UiPage::SCOPE: drawScopePage(); break;
            case UiPage::CAPTURES: drawCapturesPage(); break;
            case UiPage::NODES: drawNodesPage(); break;
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
