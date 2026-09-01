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

#include "battery.h"
#include "cell_plan.h"
#include "detection.h"
#include "discovery_plan.h"
#include "energy_plan.h"
#include "gps_task.h"
#include "logger_task.h"
#include "serial_control.h"
#include "radio_task.h"
#include "spi_bus.h"
#include "ui_labels.h"
#include "version.h"
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
        case UiPage::CHANNEL: return "CHANNEL";
        case UiPage::GPS: return "GPS";
        case UiPage::SYSTEM: return "SYSTEM";
        case UiPage::PROBE: return "PROBE";
        case UiPage::SWEEP: return "SWEEP";
        case UiPage::CELL: return "CELL";
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

    // STANDBY/Probe banner (Radio menu): rx/log/drop above
    // stay as real frozen totals rather than being replaced — the thing
    // that must not be ambiguous is "is the radio actually listening right
    // now," not the counters themselves, which are still meaningful while
    // paused. Sits in the gap between the hero column and the bottom flush
    // band, which RADIO's layout otherwise leaves empty (unlike GPS/SYSTEM,
    // which use it for their own secondary content).
    if (radioDiscoverySweepIsActive()) {
        const uint8_t current = radioDiscoveryCandidateIndex();
        const uint8_t total = radioDiscoveryCandidateCount();
        uiTft->setTextSize(2);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(2, HEADER_H + 66);
        uiTft->print("PROBE ");
        uiTft->print(current);
        uiTft->print('/');
        uiTft->print(total);
        uiTft->setTextSize(1);
        uiTft->setCursor(2, HEADER_H + 86);
        uiTft->print("WATCH PAUSED");
    } else if (radioIsTracePaused()) {
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
// space — removes an easy, silent mistake in the table.
void drawMenuRow(int16_t y, const char *rowLabel, const char *value, bool selected) {
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
}

// What an ACTION row's value column shows. Generic over every list in the
// menu (Profile's choices, System's toggles, Display's Idle-dim cycle) —
// MenuState/MenuItem are data-driven (ui_menu.h), so this stays one switch
// on MenuAction rather than one function per list.
const char *menuEntryValue(MenuAction action) {
    switch (action) {
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
        // BRIGHTNESS_UP/DOWN aren't ACTION rows, so they never reach here.
        default: return "";
    }
}

// One list of rows, at whatever depth menu.currentList() currently is —
// root, System's list, or Display's nested list all draw through this
// same function; no depth-specific draw functions.
void drawMenuList() {
    const MenuItem *list = menu.currentList();
    const uint8_t count = menu.currentCount();
    for (uint8_t i = 0; i < count; i++) {
        const MenuItem &item = list[i];
        char label[24];
        if (item.kind == ItemKind::SLIDER) {
            // Brightness is the only SLIDER row today, so this reaches
            // straight for activeBrightnessPercent rather than being fully
            // generic — revisit if a second slider is added.
            snprintf(label, sizeof(label), "Brightness: %u%%", (unsigned)activeBrightnessPercent);
            drawMenuRow(HEADER_H + 10 + i * 24, label, nullptr, menu.currentIndex() == i);
        } else if (item.items == PROFILE_GROUP_ITEMS) {
            // "Profile: Meshtastic" — surfaces the live profile without
            // drilling into the group. Goes through the same label/value
            // path as every ACTION row so drawMenuRow's ": " separator
            // stays the one place that decides the shape, rather than this
            // branch building its own copy of it.
            drawMenuRow(HEADER_H + 10 + i * 24, item.label, uiProfileLabel(radioActiveProfile()),
                        menu.currentIndex() == i);
        } else if (item.kind == ItemKind::GROUP) {
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

// Brightness slider screen — the one SLIDER view today. Large live readout
// plus a filled-bar track, same outline+fill visual language as
// drawHeapBar()/drawFreqBar() rather than a third bar style.
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
    // Enter now leaves the slider the same way ` (BACK) does (ui_menu.h's
    // handleSlider(), 2026-08-29) — hint text updated so it doesn't go
    // silently out of date the moment a real, working key isn't mentioned.
    uiTft->print(",/. adjust   Enter/` back");
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
            case UiPage::CHANNEL: drawChannelPage(); break;
            case UiPage::GPS: drawGpsPage(); break;
            case UiPage::SYSTEM: drawSystemPage(); break;
            case UiPage::PROBE: drawProbePage(); break;
            case UiPage::SWEEP: drawSweepPage(); break;
            case UiPage::CELL: drawCellPage(); break;
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
