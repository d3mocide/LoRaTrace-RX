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
constexpr size_t HEADER_BREADCRUMB_MAX_CHARS = 23;
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
        case UiPage::FOCUS: return "FOCUS";
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
// operator-facing carousel is five stops (Radio/Activity/Channel/GPS/System)
// — MAIN_PAGES in ui_task.cpp is the authority, not this comment, which said
// "four stops (Radio/Channel/GPS/System)" until 2026-09-05: written when
// Tools/Analyze moved into the menu, and not updated when Activity rejoined
// the carousel. It was read back as fact during V2 planning and used to
// contradict a correct UI proposal, so check MAIN_PAGES rather than this
// sentence. Probe/Sweep/Cell and Meter/Waterfall/Scope/Captures/Nodes are
// views of those five cards (CARD_VIEWS), not stops of their own, and the
// dots below count them. An earlier revision used raw UiPage ordinals for a
// six-stop carousel and showed e.g. Analyze as "8/14" — technically not
// wrong, but confusing enough that an operator asked "where are the other 6
// cards" (2026-09-04).
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

    // View dots: one per view this card carries, filled for the current one.
    // Without them up/down is invisible — the card looks like a single screen
    // until an operator happens to press a key that appears to do nothing on
    // four of five cards. Drawn only where there is something to cycle (System
    // has one view), so a lone dot never implies a hidden second screen.
    //
    // Centred on the footer rather than tucked beside the "N/M" carousel
    // position (operator request): they are two different axes of navigation —
    // up/down through a card's views, left/right across cards — and sitting
    // them next to each other read as one cluster. The centre is empty on
    // every page, comfortably clear of the profile label at x=2 and of "N/M"
    // at the right edge even at four views (~21px wide).
    const uint8_t views = activeViewCount();
    if (views > 1) {
        constexpr int16_t PITCH = 7;
        const int16_t x0 = (int16_t)(uiTft->width() / 2) - (int16_t)(views - 1) * PITCH / 2;
        for (uint8_t i = 0; i < views; i++) {
            const int16_t cx = x0 + (int16_t)i * PITCH;
            if (i == activeViewIndex()) {
                uiTft->fillCircle(cx, y + 3, 2, COL_FG);
            } else {
                uiTft->drawCircle(cx, y + 3, 2, COL_DIM);
            }
        }
    }
}

// Every "nothing to show yet" view, one shape: a dim size-2 headline, then up
// to two size-1 lines. Replaces ten hand-rolled copies that had drifted apart
// in offsets, colour and wording — two of them ("NO SWEEP YET" / "NO SWEEPS
// YET") on adjacent views of the same card, which is what made the
// duplication visible.
//
// `line1` is normally cardHintLine(view): the key that fills this view.
// Downstream views — Waterfall, Captures, Nodes, Meter — pass a plain
// explanation instead, because no key of theirs produces their data; saying
// what they are waiting on is more use than repeating the instruction from
// the view one press above.
void drawEmptyView(const char *headline, const char *line1, const char *line2 = nullptr) {
    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 8);
    uiTft->print(headline);
    uiTft->setTextSize(1);
    if (line1 != nullptr) {
        uiTft->setCursor(2, HEADER_H + 34);
        uiTft->print(line1);
    }
    if (line2 != nullptr) {
        uiTft->setCursor(2, HEADER_H + 46);
        uiTft->print(line2);
    }
}

// How long ago a bounded action last reached a terminal state, for the detail
// line under a held result. Replaces the four independent "revert the headline
// to a dim IDLE after RESULT_HOLD_MS" branches Probe/Sweep/Cell/Scope each
// carried: IDLE described the *radio* while the view below it was still
// showing a real result, so Activity's dashboard could show a sweep peak while
// Activity's own Sweep view claimed IDLE — two views of one card disagreeing
// about whether anything had happened. Radio's card owns radio state (its
// STANDBY word); a tool view owes the operator the age of its data instead.
// Empty string before anything has completed this power-on.
const char *resultAge(uint32_t shownAt) {
    static char buf[12];
    if (shownAt == 0) return "";
    const uint32_t sec = (millis() - shownAt) / 1000;
    if (sec < 60) {
        snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)sec);
    } else if (sec < 3600) {
        snprintf(buf, sizeof(buf), "%lum ago", (unsigned long)(sec / 60));
    } else {
        snprintf(buf, sizeof(buf), "%luh ago", (unsigned long)(sec / 3600));
    }
    return buf;
}

// A plot well: left and right rails and a floor, with the header's own
// hairline as the top edge (operator suggestion, 2026-09-06). Full-bleed on
// purpose — the rails meet the header line at x=0 and x=239, so it reads as a
// compartment carved out below the header rather than a second box floating
// 2px under it, which is what the removed band borders were.
//
// The floor doubles as the plot's baseline: bars and traces sit directly on it
// instead of floating above an implied zero. Only the three cards whose band is
// a plot use it. Radio's six cells and Channel's axis track are already bounded
// forms, and a well around a container is the doubling this replaced.
void drawPlotWell(int16_t top, int16_t floorY) {
    const int16_t w = uiTft->width();
    uiTft->drawFastVLine(0, top, (int16_t)(floorY - top), COL_DIM);
    uiTft->drawFastVLine(w - 1, top, (int16_t)(floorY - top), COL_DIM);
    uiTft->drawFastHLine(0, floorY, w, COL_DIM);
}

// --- Bounded-action result skeleton ---------------------------------------
// Probe, Sweep, Cell and Focus all report the same shape of thing: one headline
// value, a terminal state, progress through a bounded run, and a small summary.
// Focus grew the best version of that layout (Workstream 12) and the other
// three were each carrying their own arrangement of the same parts, so this is
// Focus's skeleton lifted out for all four (operator request, 2026-09-06).
//
// Row 1  hero (size 2) + a mid detail + right-aligned status word
// Row 2  full-width progress bar
// Row 3  left and right detail labels
// ...    each page's own content block
// Row 5  three colour-coded summary values

void drawActionHero(const char *hero, const char *mid, uint16_t midCol, const char *status,
                    uint16_t statusCol) {
    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 4);
    uiTft->print(hero);

    uiTft->setTextSize(1);
    if (mid != nullptr) {
        uiTft->setTextColor(midCol, COL_BG);
        uiTft->setCursor(138, HEADER_H + 6);
        uiTft->print(mid);
    }
    uiTft->setTextColor(statusCol, COL_BG);
    uiTft->setCursor(238 - (int16_t)strlen(status) * 6, HEADER_H + 6);
    uiTft->print(status);
}

void drawActionBar(uint32_t done, uint32_t total, uint16_t col) {
    constexpr int16_t GX = 2, GW = 236;
    const int16_t y = HEADER_H + 28;
    uiTft->drawRect(GX, y, GW, 8, COL_DIM);
    const uint32_t span = total ? total : 1;
    const uint32_t clamped = done > span ? span : done;
    const int16_t fill = (int16_t)((GW - 2) * clamped / span);
    if (fill > 0) uiTft->fillRect(GX + 1, y + 1, fill, 6, col);
}

void drawActionLabels(const char *left, const char *right) {
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 39);
    uiTft->print(left);
    if (right != nullptr) {
        uiTft->setCursor(238 - (int16_t)strlen(right) * 6, HEADER_H + 39);
        uiTft->print(right);
    }
}

// Three summary values on one row, at Focus's own P50/P90/PK columns.
void drawActionStats(const char *a, uint16_t ca, const char *b, uint16_t cb, const char *c,
                     uint16_t cc) {
    uiTft->setTextSize(1);
    uiTft->setTextColor(ca, COL_BG);
    uiTft->setCursor(2, HEADER_H + 95);
    uiTft->print(a);
    uiTft->setTextColor(cb, COL_BG);
    uiTft->setCursor(86, HEADER_H + 95);
    uiTft->print(b);
    uiTft->setTextColor(cc, COL_BG);
    uiTft->setCursor(164, HEADER_H + 95);
    uiTft->print(c);
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

// One boxed card: title, size-2 value, size-1 subtitle. Arrived as Activity's
// own private helper and became the shared idiom when Radio was rebuilt in the
// same language (2026-09-06) — a card carries a visual band across the top and
// a row of three of these under it. `valueCol` defaults to COL_FG; Radio's
// state and storage cards colour it to carry health.
constexpr int16_t STAT_CARD_H = 60;
// 3 cards + 2 gaps span the full 240: 78*3 + 3*2 = 240, so the outer card
// edges land on x=0 and x=239 — flush with the plot well's rails above them
// (operator request, 2026-09-06). Radio's six grid cells share this geometry.
constexpr int16_t STAT_CARD_W = 78;

void statCard(int16_t x, int16_t y, int16_t w, const char *title, uint16_t titleCol,
              const char *value, const char *sub, uint16_t valueCol = COL_FG) {
    uiTft->drawRect(x, y, w, STAT_CARD_H, COL_DIM);
    uiTft->setTextSize(1);
    uiTft->setTextColor(titleCol, COL_BG);
    uiTft->setCursor(x + 4, y + 4);
    uiTft->print(title);
    // 5 characters is what fits at size 2 inside the 4px left inset; longer
    // values drop to size 1 rather than running out through the border. Cheap
    // insurance for every caller — Activity's AWAY T card would already
    // overflow on a sweep past 100s ("120.5s") without it.
    const bool wide = strlen(value) > 5;
    uiTft->setTextSize(wide ? 1 : 2);
    uiTft->setTextColor(valueCol, COL_BG);
    uiTft->setCursor(x + 4, y + (wide ? 24 : 18));
    uiTft->print(value);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(x + 4, y + 44);
    // Clamped to the box, like the value above it. A size-1 character is 6px
    // and the text starts 4px in, so a 77px card holds 11 — "Enter resumes"
    // was 13 and bled into the neighbouring card on real hardware. Callers
    // should still write something that fits; this only stops an overrun from
    // corrupting the card beside it.
    char fitted[20];
    const size_t maxChars = (size_t)((w - 6) / 6);
    snprintf(fitted, sizeof(fitted), "%.*s", (int)(maxChars < sizeof(fitted) - 1 ? maxChars : sizeof(fitted) - 1), sub);
    uiTft->print(fitted);
}

// x of the Nth stat card in the standard three-across row.
constexpr int16_t statCardX(uint8_t n) { return (int16_t)(n * (STAT_CARD_W + 3)); }

// One cell of Radio's 2x3 grid: a label and its count, boxed. Counts are
// neutral; a loss figure carries colour, green at zero and red otherwise,
// because 0 is the only good value and nobody should have to compare two
// numbers to notice a leak.
void radioCell(int16_t x, int16_t y, const char *label, uint32_t v, bool isLoss, bool flash) {
    char buf[12];
    // No box (operator request, 2026-09-06): six cell borders plus three card
    // borders made nine rectangles on one screen, and the chrome became the
    // figure while the numbers became the ground. The grid still reads as a
    // grid from column alignment and the well around it — the boxes were
    // carrying nothing the layout was not already saying.
    uiTft->setTextSize(1);
    uiTft->setTextColor(flash ? COL_GOOD : COL_DIM, COL_BG);
    uiTft->setCursor(x + 4, y);
    uiTft->print(label);
    uiTft->setTextColor(isLoss ? (v == 0 ? COL_GOOD : COL_BAD) : COL_FG, COL_BG);
    uiTft->setCursor(x + 4, y + 9);
    if (v < 100000UL) {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
    } else {
        snprintf(buf, sizeof(buf), "%luk", (unsigned long)(v / 1000UL));
    }
    uiTft->print(buf);
}

// Radio, view 1 of 3 (Meter and Scope are 2 and 3). The receive chain as six
// facts rather than three composite stages with arrows between them (operator
// request, 2026-09-06): the arrows carried causality but cost every stage its
// own box, and the result read as one dense object instead of six readable
// ones. Row 1 is what the radio did, row 2 is what the pipeline did with it,
// so the chain still reads in order without being drawn.
void drawRadioPage() {
    // The same well the plot cards use, so Radio's band is bounded the way
    // theirs are without every value inside it being boxed too.
    drawPlotWell(HEADER_H, HEADER_H + 45);
    constexpr int16_t ROW1 = HEADER_H + 4, ROW2 = HEADER_H + 26;
    radioCell(statCardX(0), ROW1, "RX", radioPacketCount(), false, rxPulseActive());
    radioCell(statCardX(1), ROW1, "CRC", radioCrcErrorCount(), true, false);
    radioCell(statCardX(2), ROW1, "MISS", radioBusMissCount(), true, false);
    radioCell(statCardX(0), ROW2, "QUEUE", radioQueueDropCount(), true, false);
    radioCell(statCardX(1), ROW2, "LOG", loggerRowsWritten(), false, false);
    radioCell(statCardX(2), ROW2, "DROP", loggerRowsDropped() + loggerScanRowsDropped(), true, false);

    const int16_t cy = HEADER_H + 49;
    char value[14], sub[16];

    // STATE answers "am I even listening", which this page could not say
    // before: its old bare STANDBY word conflated an operator pause with a
    // bounded action holding the radio, and showed nothing at all in the
    // second case. Those are different situations — one is waiting on you,
    // the other resolves itself — so they get different words.
    const char *awayWho = nullptr;
    if (radioDiscoverySweepIsActive()) awayWho = "probe";
    else if (radioEnergySweepIsActive()) awayWho = "sweep";
    else if (radioCellSweepIsActive()) awayWho = "cell";
    else if (radioScopeAcquireIsActive()) awayWho = "scope";
    else if (radioFocusSurveyIsActive()) awayWho = "focus";

    // The value word carries the state; the sub names what that state is about
    // — the profile being watched or paused, or the tool that took the radio.
    // It used to restate the value ("sweep running", "Enter resumes"), which
    // was both redundant and too wide for the card (operator report).
    uint16_t stateCol;
    snprintf(sub, sizeof(sub), "%s", awayWho != nullptr ? awayWho
                                                        : uiProfileLabel(radioActiveProfile()));
    if (awayWho != nullptr) {
        snprintf(value, sizeof(value), "AWAY");
        stateCol = COL_WARN;
    } else if (radioIsTracePaused()) {
        snprintf(value, sizeof(value), "STANDBY");
        stateCol = COL_WARN;
    } else {
        snprintf(value, sizeof(value), "WATCH");
        stateCol = COL_GOOD;
    }
    statCard(statCardX(0), cy, STAT_CARD_W, "STATE", stateCol, value, sub, stateCol);

    const bool sdReady = loggerSdReady();
    snprintf(value, sizeof(value), "%s", sdReady ? "ok" : "DOWN");
    snprintf(sub, sizeof(sub), "run %u", (unsigned)loggerRunIndex());
    statCard(statCardX(1), cy, STAT_CARD_W, "STORAGE", sdReady ? COL_GOOD : COL_BAD, value, sub,
             sdReady ? COL_FG : COL_BAD);

    // Worst flush, not the flush count: this is the number that decides
    // whether BATCH_BUF_SIZE needs retuning (docs/DESIGN.md 8.2), and bus
    // misses are the same SPI-contention story one layer down, so they share
    // a card rather than sitting in separate columns.
    // MISS moved into the grid above, so this card carries the flush story on
    // its own: the worst hold is what decides whether BATCH_BUF_SIZE needs
    // retuning (docs/DESIGN.md 8.2), and the count says how often it happens.
    snprintf(value, sizeof(value), "%lums", (unsigned long)loggerMaxFlushMs());
    snprintf(sub, sizeof(sub), "bus %lu", (unsigned long)spiBusContentionCount());
    statCard(statCardX(2), cy, STAT_CARD_W, "BUS", COL_DIM, value, sub);
}

// Probe, on the shared bounded-action skeleton (2026-09-06). The most
// overdue of the four: it was still a headline word over a stack of text
// lines, with the answer -- which candidate tuples actually heard something --
// last on the page. Now the hit count is the hero, and the named candidates
// take the content block where Sweep puts occupancy and Focus its gauge.
void drawProbePage() {
    const DiscoverySweepState state = radioDiscoverySweepState();
    const uint8_t done = radioDiscoveryCandidateIndex();
    const uint8_t total = radioDiscoveryCandidateCount();
    const uint16_t hits = radioDiscoveryCadDetectedCount();
    const uint16_t freeCount = radioDiscoveryCadFreeCount();
    const uint16_t timeouts = radioDiscoveryCadTimeoutCount();
    const uint16_t errors = radioDiscoveryErrorCount();

    if (state == DiscoverySweepState::IDLE) {
        drawEmptyView("NO PROBE YET", cardHintLine(UiPage::PROBE));
        return;
    }

    const bool running = state == DiscoverySweepState::RUNNING;
    const bool holdExpired = !running && probeTerminalShownAt != 0 &&
                             millis() - probeTerminalShownAt >= RESULT_HOLD_MS;
    const char *status;
    uint16_t statusCol;
    if (running) {
        status = "SCANNING"; statusCol = COL_WARN;
    } else if (state == DiscoverySweepState::COMPLETE) {
        status = "COMPLETE"; statusCol = holdExpired ? COL_DIM : COL_GOOD;
    } else if (state == DiscoverySweepState::CANCELLED) {
        status = "CANCELLED"; statusCol = holdExpired ? COL_DIM : COL_WARN;
    } else {
        status = "FAILED"; statusCol = COL_BAD;
    }

    // The hero is the finding, not the progress: "2 of 8 candidate channels
    // had energy" is what an operator ran this for.
    char hero[20], mid[16], left[32], right[24];
    snprintf(hero, sizeof(hero), "%u / %u", (unsigned)hits, (unsigned)total);
    snprintf(mid, sizeof(mid), "ACTIVE");
    drawActionHero(hero, mid, hits > 0 ? COL_WARN : COL_DIM, status, statusCol);

    drawActionBar(done, total, running ? COL_WARN : COL_GOOD);
    snprintf(left, sizeof(left), running ? "TESTING %u / %u" : "TARGETS %u / %u", (unsigned)done,
             (unsigned)total);
    if (running) {
        snprintf(right, sizeof(right), "Watch away");
    } else {
        snprintf(right, sizeof(right), "%lums  %s", (unsigned long)radioDiscoveryLastAwayMs(),
                 resultAge(probeTerminalShownAt));
    }
    drawActionLabels(left, right);

    // Content block: which named candidates hit. Mask bit i is
    // plan.candidates[i] -- radio_task.cpp's raw loop index into the candidate
    // table, not the skip-adjusted done/total above (see
    // radioDiscoveryCadDetectedMask()'s own doc comment).
    uiTft->drawRect(2, HEADER_H + 59, 236, 32, COL_DIM);
    uiTft->setTextSize(1);
    const DiscoveryPlan plan = discoveryPlanForProfile(radioActiveProfile());
    const uint32_t mask = radioDiscoveryCadDetectedMask();
    uint8_t shown = 0;
    for (uint8_t i = 0; i < plan.count && shown < 3; i++) {
        if ((mask & (1UL << i)) == 0) continue;
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(6, HEADER_H + 63 + shown * 9);
        uiTft->print(uiDiscoveryCandidateLabel(plan.candidates[i]));
        shown++;
    }
    if (shown == 0) {
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(6, HEADER_H + 63);
        uiTft->print(running ? "listening..." : "no candidate heard energy");
    }

    char a[16], b[16], c[16];
    snprintf(a, sizeof(a), "HIT %u", (unsigned)hits);
    snprintf(b, sizeof(b), "FREE %u", (unsigned)freeCount);
    if (errors > 0) {
        snprintf(c, sizeof(c), "ERR %u", (unsigned)errors);
    } else {
        snprintf(c, sizeof(c), "T/O %u", (unsigned)timeouts);
    }
    drawActionStats(a, hits > 0 ? COL_WARN : COL_DIM, b, COL_DIM, c, errors > 0 ? COL_BAD : COL_DIM);
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
// Sweep, on the shared bounded-action skeleton (2026-09-06). Its own layout
// predated Focus's and carried the same parts in a different arrangement:
// headline word top-left, progress buried in a text line, results split
// between a left column and right-hand statBlocks. Now the frequency is the
// hero it always should have been — a sweep's answer is "where", not "how far
// along" — with the state word right-aligned as a status, not a headline.
void drawSweepPage() {
    const EnergySweepState state = radioEnergySweepState();
    const uint16_t bin = radioEnergyBinIndex();
    const uint16_t total = radioEnergyBinCount();
    const uint16_t peaks = radioEnergyPeakCount();
    const bool repeating = radioEnergySweepRepeatIsActive();
    const EnergySweepBand band = energySweepBandForRegion(radioEnergySweepRegion());

    if (state == EnergySweepState::IDLE) {
        drawEmptyView("NO SWEEP YET", cardHintLine(UiPage::SWEEP));
        return;
    }

    const bool running = state == EnergySweepState::RUNNING;
    // Repeat mode outranks the terminal word: back-to-back laps would otherwise
    // flicker SCANNING/COMPLETE every single one, which reads as broken rather
    // than as one continuous ambient scan.
    const bool holdExpired = !repeating && !running && sweepTerminalShownAt != 0 &&
                             millis() - sweepTerminalShownAt >= RESULT_HOLD_MS;
    const char *status;
    uint16_t statusCol;
    if (repeating) {
        status = "REPEATING"; statusCol = COL_WARN;
    } else if (running) {
        status = "SCANNING"; statusCol = COL_WARN;
    } else if (state == EnergySweepState::COMPLETE) {
        status = "COMPLETE"; statusCol = holdExpired ? COL_DIM : COL_GOOD;
    } else if (state == EnergySweepState::CANCELLED) {
        status = "CANCELLED"; statusCol = holdExpired ? COL_DIM : COL_WARN;
    } else {
        status = "FAILED"; statusCol = COL_BAD;
    }

    // While running the hero tracks the bin being measured; once finished it
    // becomes the strongest peak, which is the result an operator came for.
    const EnergyStrongestPeak strongest = radioEnergyStrongestPeak();
    char hero[20], mid[16], left[32], right[24];
    if (!running && strongest.valid) {
        snprintf(hero, sizeof(hero), "%.3f MHz", (double)strongest.freq_mhz);
    } else {
        snprintf(hero, sizeof(hero), "%.3f MHz",
                 (double)energyBinFrequencyMhz(bin, band, ENERGY_SWEEP_DEFAULT_STEP));
    }
    snprintf(mid, sizeof(mid), "BIN %u", (unsigned)bin);
    drawActionHero(hero, mid, COL_WARN, status, statusCol);

    drawActionBar(bin, total, running ? COL_WARN : COL_GOOD);
    snprintf(left, sizeof(left), running ? "SCANNING %u / %u" : "BINS %u / %u", (unsigned)bin,
             (unsigned)total);
    if (running) {
        snprintf(right, sizeof(right), "Watch away");
    } else {
        snprintf(right, sizeof(right), "%lums  %s", (unsigned long)radioEnergyLastAwayMs(),
                 resultAge(sweepTerminalShownAt));
    }
    drawActionLabels(left, right);

    // Occupancy strip in the content block, where Focus puts its gauge: every
    // bin the last completed sweep marked as a peak, positioned across the band.
    uiTft->drawRect(2, HEADER_H + 59, 236, 20, COL_DIM);
    drawSweepOccupancy(4, HEADER_H + 62, 232, total);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(4, HEADER_H + 83);
    uiTft->print((int)band.lo_mhz);
    char hiBuf[8];
    snprintf(hiBuf, sizeof(hiBuf), "%d", (int)band.hi_mhz);
    uiTft->setCursor(236 - (int16_t)strlen(hiBuf) * 6, HEADER_H + 83);
    uiTft->print(hiBuf);

    char a[16], b[16], c[16];
    snprintf(a, sizeof(a), "PEAKS %u", (unsigned)peaks);
    if (strongest.valid) {
        snprintf(b, sizeof(b), "PK %d", (int)(strongest.rssi_peak_dbm_x10 / 10));
    } else {
        snprintf(b, sizeof(b), "PK --");
    }
    if (repeating) {
        snprintf(c, sizeof(c), "LAP %lu", (unsigned long)radioEnergySweepRepeatCount());
    } else if (state == EnergySweepState::FAILED) {
        snprintf(c, sizeof(c), "ERR %d", radioLastError());
    } else {
        snprintf(c, sizeof(c), "MGN %.0fdB", (double)radioEnergySweepMarginDbmX10() / 10.0);
    }
    drawActionStats(a, peaks == 0 ? COL_DIM : COL_WARN, b, COL_FG, c,
                    state == EnergySweepState::FAILED ? COL_BAD : COL_DIM);
}

// One marker inside the bracket, with its label above the box.
// Focus (V2 Workstream 12). Geometry follows drawFocusScreen() in
// docs/UI-Recommendations.html: size-2 target frequency with the bin and
// status on the same row, a full-width dwell bar, a boxed -120..-40 dBm
// bracket with the summary values marked inside it, and a stats readout.
//
// Two deliberate departures from that spec. The "Operator Truth Badge"
// (SAMPLING/REPEATED/INSUFFICIENT, "High confidence") is absent: its
// thresholds are unselected and a single confidence word is what §3 of the
// design forbids substituting for the counts. And the guide's third statistic
// is a noise Floor, which a pass does not measure -- FocusObservation carries
// median, P90 and peak -- so peak takes that slot, which is also the value the
// 2026-09-05 field measurements showed actually separates a source from
// ambient.
constexpr int16_t FOCUS_SCALE_MIN_DBM = -120;
constexpr int16_t FOCUS_SCALE_SPAN_DB = 80;   // -120 .. -40
int16_t focusScaleX(int16_t dbm_x10, int16_t x, int16_t w) {
    const float dbm = (float)dbm_x10 / 10.0f;
    const float lo = (float)FOCUS_SCALE_MIN_DBM;
    const float hi = lo + (float)FOCUS_SCALE_SPAN_DB;
    const float clamped = dbm < lo ? lo : (dbm > hi ? hi : dbm);
    return (int16_t)(x + (clamped - lo) / (float)FOCUS_SCALE_SPAN_DB * (float)w);
}

void drawFocusMarker(int16_t dbm_x10, int16_t x, int16_t w, int16_t boxY,
                     uint16_t colour, const char *label) {
    if (dbm_x10 == FOCUS_RSSI_NO_SAMPLE_DBM_X10) return;
    const int16_t mx = focusScaleX(dbm_x10, x, w);
    uiTft->fillRect(mx - 1, boxY + 1, 3, 18, colour);
    uiTft->setTextSize(1);
    uiTft->setTextColor(colour, COL_BG);
    int16_t lx = mx - 10;
    if (lx < 0) lx = 0;
    if (lx > 240 - 24) lx = 240 - 24;
    uiTft->setCursor(lx, boxY - 10);
    uiTft->print(label);
}

// Phase 11 Cell result card (2026-09-01), same layout shape as
// drawSweepPage() above — this is the same bounded-bin-sweep pattern, just
// scoped to cell_plan.h's 101-bin, 869-894MHz band instead of the full
// front end, and with no CAD/Pass-B step (cell_plan.h's file header: CAD
// never fires on a cellular carrier, so there is nothing to attempt).
// drawFreqBar() is called with Cell's own band bounds, not the default
// 868-923MHz, so the marker resolves position within the actual band being
// swept instead of a barely-visible sliver of the full front end. The
// disclaimer line stays on screen deliberately: docs/DESIGN.md §5a's central
// rule is that a cell-band RSSI reading is never presented as anything more
// than presence/strength — no cell ID, no decode, no tower location. (Sweep
// carried a matching "listening to the noise" line until 2026-09-06; that one
// was flavour, not a truthfulness claim, and went when the empty-state pass
// wanted the row. This one stays.)
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

void drawFocusPage() {
    const FocusRuntimeState state = radioFocusSurveyState();
    FocusObservation last;
    const bool haveResult = radioFocusLastObservation(last);
    const bool running = state == FocusRuntimeState::SURVEYING ||
                         state == FocusRuntimeState::RESTORING;

    if (state == FocusRuntimeState::IDLE && !haveResult) {
        // Naming the frequency Enter would actually survey, rather than only
        // the rule that picks it (ui_actions.cpp's selectFocusRequest()): after
        // a sweep this is the same peak Activity's own SWEEP PK card is
        // showing, which is what makes the sweep-then-focus loop legible
        // instead of something the operator has to take on trust.
        const EnergyStrongestPeak peak = radioEnergyStrongestPeak();
        char target[40];
        if (peak.valid) {
            snprintf(target, sizeof(target), "target %.3f MHz (Sweep peak)", (double)peak.freq_mhz);
        } else {
            snprintf(target, sizeof(target), "target %.3f MHz (home, no sweep)",
                     (double)radioActiveChannel().freq_mhz);
        }
        drawEmptyView("NO SURVEY YET", cardHintLine(UiPage::FOCUS), target);
        return;
    }

    char buf[32];

    // Row 1: target frequency (size 2), bin, and state.
    uiTft->setTextSize(2);
    uiTft->setCursor(2, HEADER_H + 4);
    uiTft->setTextColor(COL_FG, COL_BG);
    snprintf(buf, sizeof(buf), "%.3f MHz", (double)last.freq_mhz);
    uiTft->print(buf);

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_WARN, COL_BG);
    uiTft->setCursor(138, HEADER_H + 6);
    // "BIN n" rather than the guide's "[CH n]": this is a Sweep bin index, and
    // dropping the brackets keeps a 3-digit bin clear of the right-aligned
    // status, which can be 9 characters ("SURVEYING").
    snprintf(buf, sizeof(buf), "BIN %u", (unsigned)last.selection_bin_index);
    uiTft->print(buf);

    // A survey that did not restore Watch takes the status slot outright: it
    // is the one outcome an operator must not miss, and it is not implied by
    // the terminal state (a COMPLETE can still fail to restore).
    const char *status;
    uint16_t statusCol;
    if (!running && !last.home_restore) {
        status = "NO HOME"; statusCol = COL_BAD;
    } else {
        switch (state) {
            case FocusRuntimeState::SURVEYING: status = "SURVEYING"; statusCol = COL_WARN; break;
            case FocusRuntimeState::RESTORING: status = "RESTORING"; statusCol = COL_WARN; break;
            case FocusRuntimeState::COMPLETE:  status = "COMPLETE";  statusCol = COL_GOOD; break;
            case FocusRuntimeState::CANCELLED: status = "CANCELLED"; statusCol = COL_WARN; break;
            case FocusRuntimeState::TIMEOUT:   status = "TIMEOUT";   statusCol = COL_WARN; break;
            case FocusRuntimeState::FAILED:    status = "FAILED";    statusCol = COL_BAD;  break;
            default:                           status = "IDLE";      statusCol = COL_DIM;  break;
        }
    }
    uiTft->setTextColor(statusCol, COL_BG);
    uiTft->setCursor(238 - (int16_t)strlen(status) * 6, HEADER_H + 6);
    uiTft->print(status);

    // Dwell bar, full width.
    constexpr int16_t GW = 236, GX = 2;
    const int16_t barY = HEADER_H + 28;
    uiTft->drawRect(GX, barY, GW, 8, COL_DIM);
    const uint32_t total = last.requested_dwell_ms ? last.requested_dwell_ms : 1;
    const uint32_t done = last.observation_ms > total ? total : last.observation_ms;
    const int16_t fill = (int16_t)((GW - 2) * done / total);
    if (fill > 0) uiTft->fillRect(GX + 1, barY + 1, fill, 6, COL_GOOD);

    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 39);
    snprintf(buf, sizeof(buf), "DWELL %.1fs / %.1fs", done / 1000.0, total / 1000.0);
    uiTft->print(buf);
    snprintf(buf, sizeof(buf), "%u SAMPLES", (unsigned)last.sample_count);
    uiTft->setCursor(238 - (int16_t)strlen(buf) * 6, HEADER_H + 39);
    uiTft->print(buf);

    // Boxed bracket gauge with the summary values marked inside it.
    //
    // Sits 5px lower than the guide's HEADER_H+54. The marker labels are drawn
    // above the box (boxY - 10), which at +54 lands on the DWELL/SAMPLES row
    // ending at +47 and overprints it. There is spare height at the bottom of
    // the plate, so the gauge and everything below it move down instead of
    // shrinking the labels.
    const int16_t boxY = HEADER_H + 59;
    uiTft->drawRect(GX, boxY, GW, 20, COL_DIM);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(4, HEADER_H + 83);   uiTft->print("-120");
    uiTft->setCursor(110, HEADER_H + 83); uiTft->print("-80");
    uiTft->setCursor(220, HEADER_H + 83); uiTft->print("-40");

    drawFocusMarker(last.rssi_median_dbm_x10, GX, GW, boxY, COL_GOOD, "P50");
    drawFocusMarker(last.rssi_p90_dbm_x10, GX, GW, boxY, COL_WARN, "P90");
    drawFocusMarker(last.rssi_peak_dbm_x10, GX, GW, boxY, COL_FG, "PK");

    // Stats readout.
    char v[12];
    uiTft->setTextColor(COL_GOOD, COL_BG);
    focusFormatRssiOrBlank(last.rssi_median_dbm_x10, v, sizeof(v));
    uiTft->setCursor(2, HEADER_H + 95);
    snprintf(buf, sizeof(buf), "P50 %s", v[0] ? v : "--");
    uiTft->print(buf);

    uiTft->setTextColor(COL_WARN, COL_BG);
    focusFormatRssiOrBlank(last.rssi_p90_dbm_x10, v, sizeof(v));
    uiTft->setCursor(86, HEADER_H + 95);
    snprintf(buf, sizeof(buf), "P90 %s", v[0] ? v : "--");
    uiTft->print(buf);

    uiTft->setTextColor(COL_FG, COL_BG);
    focusFormatRssiOrBlank(last.rssi_peak_dbm_x10, v, sizeof(v));
    uiTft->setCursor(164, HEADER_H + 95);
    snprintf(buf, sizeof(buf), "PK %s", v[0] ? v : "--");
    uiTft->print(buf);
}

// Cell, on the shared bounded-action skeleton (2026-09-06) — the same layout
// as Sweep and Probe, since it is the same kind of run. The strongest carrier
// is the hero: a cell-band scan's answer is which frequency was loudest, and
// that was previously a right-column statBlock two rows down from the state
// word that had the headline.
void drawCellPage() {
    const CellSweepState state = radioCellSweepState();
    const uint16_t bin = radioCellBinIndex();
    const uint16_t total = radioCellBinCount();
    const bool repeating = radioCellSweepRepeatIsActive();

    if (state == CellSweepState::IDLE) {
        drawEmptyView("NO SCAN YET", cardHintLine(UiPage::CELL));
        return;
    }

    const bool running = state == CellSweepState::RUNNING;
    const bool holdExpired = !repeating && !running && cellTerminalShownAt != 0 &&
                             millis() - cellTerminalShownAt >= RESULT_HOLD_MS;
    const char *status;
    uint16_t statusCol;
    if (repeating) {
        status = "REPEATING"; statusCol = COL_WARN;
    } else if (running) {
        status = "SCANNING"; statusCol = COL_WARN;
    } else if (state == CellSweepState::COMPLETE) {
        status = "COMPLETE"; statusCol = holdExpired ? COL_DIM : COL_GOOD;
    } else if (state == CellSweepState::CANCELLED) {
        status = "CANCELLED"; statusCol = holdExpired ? COL_DIM : COL_WARN;
    } else {
        status = "FAILED"; statusCol = COL_BAD;
    }

    const CellStrongestSignal strongest = radioCellStrongestSignal();
    char hero[20], mid[16], left[32], right[24];
    if (!running && strongest.valid) {
        snprintf(hero, sizeof(hero), "%.1f MHz", (double)strongest.freq_mhz);
    } else {
        snprintf(hero, sizeof(hero), "%.1f MHz", (double)cellBinFrequencyMhz(bin));
    }
    snprintf(mid, sizeof(mid), "BIN %u", (unsigned)bin);
    drawActionHero(hero, mid, COL_WARN, status, statusCol);

    drawActionBar(bin, total, running ? COL_WARN : COL_GOOD);
    snprintf(left, sizeof(left), running ? "SCANNING %u / %u" : "BINS %u / %u", (unsigned)bin,
             (unsigned)total);
    if (running) {
        snprintf(right, sizeof(right), "Watch away");
    } else {
        snprintf(right, sizeof(right), "%lums  %s", (unsigned long)radioCellLastAwayMs(),
                 resultAge(cellTerminalShownAt));
    }
    drawActionLabels(left, right);

    // FCC A/B block markers in the content block, where Sweep puts occupancy.
    uiTft->drawRect(2, HEADER_H + 59, 236, 20, COL_DIM);
    drawCellBandBlocks(4, HEADER_H + 64, 232);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(4, HEADER_H + 83);
    uiTft->print((int)CELL_SWEEP_BAND_LO_MHZ);
    uiTft->setCursor(214, HEADER_H + 83);
    uiTft->print((int)CELL_SWEEP_BAND_HI_MHZ);

    // Permanent honesty line, not a one-time disclaimer: docs/DESIGN.md S5a's
    // central rule is that a cell-band RSSI reading is never presented as more
    // than presence and strength -- no cell ID, no decode, no tower location.
    char a[16], b[16], c[20];
    if (strongest.valid) {
        snprintf(a, sizeof(a), "PK %d", (int)(strongest.rssi_peak_dbm_x10 / 10));
    } else {
        snprintf(a, sizeof(a), "PK --");
    }
    if (repeating) {
        snprintf(b, sizeof(b), "LAP %lu", (unsigned long)radioCellSweepCount());
    } else if (state == CellSweepState::FAILED) {
        snprintf(b, sizeof(b), "ERR %d", radioLastError());
    } else {
        snprintf(b, sizeof(b), "RSSI only");
    }
    snprintf(c, sizeof(c), "no decode");
    drawActionStats(a, strongest.valid ? COL_WARN : COL_DIM, b,
                    state == CellSweepState::FAILED ? COL_BAD : COL_DIM, c, COL_DIM);
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
// Channel's band map, drawn against the plot well's own floor (2026-09-06).
// The map used to carry its own 10px track rect; with the well around it that
// was a box inside a box, so the floor became the frequency axis instead and
// the ticks rise from it. Green ticks are frequencies that decoded a packet
// (capture ring), amber are peaks the last completed Sweep found.
//
// This is the card's whole argument. "Am I on the right channel" was
// previously unanswerable here — the old 108px freq bar showed position within
// the front end and nothing else, so a channel with all the energy 4MHz away
// looked identical to one sitting on top of it.
void drawChannelBandMap(int16_t x, int16_t w, int16_t floorY, float tunedMhz) {
    constexpr float LO = 868.0f, HI = 928.0f;
    auto frac = [](float mhz) {
        float f = (mhz - LO) / (HI - LO);
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    };
    constexpr int16_t TICK_H = 10;

    // Sweep peaks first, so a capture tick on the same bin draws over it — a
    // decode is stronger evidence than energy, and should win the pixel.
    // Bins are resolved against the region the completed sweep actually used;
    // against the current one, changing Region drew the old band's peaks at
    // new frequencies (audit A04/A20).
    SweepSnapshot sweep;
    if (radioSweepSnapshot(sweep)) {
        const EnergySweepBand band = energySweepBandForRegion(sweep.region);
        for (uint16_t b = 0; b < sweep.bin_count; b++) {
            if (!sweepBit(sweep.peaks, b)) continue;
            const float mhz = energyBinFrequencyMhz(b, band, sweep.step);
            uiTft->drawFastVLine(x + (int16_t)((w - 1) * frac(mhz)), floorY - TICK_H, TICK_H,
                                 COL_WARN);
        }
    }

    static CaptureHistory history;
    if (analyzerCaptureHistorySnapshot(history, pdMS_TO_TICKS(20))) {
        CaptureSummary entry;
        for (uint8_t i = 0; i < history.count; i++) {
            if (!captureHistoryEntryAt(history, i, entry)) break;
            uiTft->drawFastVLine(x + (int16_t)((w - 1) * frac(entry.freq_mhz)), floorY - TICK_H,
                                 TICK_H, COL_GOOD);
        }
    }

    // The tuned marker runs taller than the data ticks and is 3px wide, so it
    // reads as "you are here" rather than as one more tick among them.
    const int16_t mx = x + (int16_t)((w - 1) * frac(tunedMhz));
    uiTft->fillRect(mx - 1, floorY - TICK_H - 6, 3, TICK_H + 6, COL_FG);
}

// Channel, view 1 of 4 (Captures, Nodes and Probe are 2-4). Redesigned
// 2026-09-06 (docs/research/2026-09-06-og-card-redesigns.html, option A) into
// the band-plus-three-cards language Activity and Radio share. It was the
// weakest card on the device: four lines that never changed unless you
// switched profile — a label, not an instrument.
void drawChannelPage() {
    const ChannelParams ch = radioActiveChannel();

    // A 28px well, not the shared 45 (2026-09-06). Nothing requires every card
    // to spend the same height on its visual: this one is an axis with ticks on
    // it, not a plot, and it was padding 15px of nothing to match its
    // neighbours. The reclaimed space goes to the frequency hero below — the
    // one number this card exists to state, and the last thing that should have
    // been demoted into a 78px tile.
    //
    // Same rails-and-floor treatment as the plot cards (operator request): the
    // floor doubles as the frequency axis the ticks rise from, which is why the
    // map no longer draws a track of its own.
    constexpr int16_t FLOOR_Y = HEADER_H + 28;
    drawPlotWell(HEADER_H, FLOOR_Y);
    drawChannelBandMap(6, 228, FLOOR_Y, ch.freq_mhz);

    // Axis labels sit at the top of the well, clear of the ticks rising from
    // the floor below them.
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(4, HEADER_H + 3);
    uiTft->print("868");
    uiTft->setCursor(112, HEADER_H + 3);
    uiTft->print("898");
    uiTft->setCursor(219, HEADER_H + 3);
    uiTft->print("928");

    uint16_t peaks = 0;
    SweepSnapshot sweep;
    if (radioSweepSnapshot(sweep)) {
        for (uint16_t b = 0; b < sweep.bin_count; b++) {
            if (sweepBit(sweep.peaks, b)) peaks++;
        }
    }

    // Hero row: the tuned frequency, with the sweep's peak count riding the
    // same baseline rather than taking a header row inside the band.
    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, HEADER_H + 31);
    uiTft->print(ch.freq_mhz, 3);
    uiTft->print(" MHz");

    char buf[20];
    snprintf(buf, sizeof(buf), "%u peak%s", (unsigned)peaks, peaks == 1 ? "" : "s");
    uiTft->setTextSize(1);
    uiTft->setTextColor(peaks == 0 ? COL_DIM : COL_WARN, COL_BG);
    uiTft->setCursor(238 - (int16_t)strlen(buf) * 6, HEADER_H + 35);
    uiTft->print(buf);

    const int16_t cy = HEADER_H + 49;
    char value[14], sub[16];

    snprintf(value, sizeof(value), "SF%u", (unsigned)ch.sf);
    snprintf(sub, sizeof(sub), "BW%.0f", (double)ch.bw_khz);
    statCard(statCardX(0), cy, STAT_CARD_W, "MODE", COL_GOOD, value, sub);

    // Decoded packets are counted for the whole power-on, not per channel —
    // the radio keeps one counter and a profile switch does not reset it. The
    // title says DECODED rather than YIELD for exactly that reason: on a card
    // about this channel, "yield" would claim an attribution the number cannot
    // support.
    static NodeRoster roster;
    uint8_t nodes = 0;
    if (analyzerNodeRosterSnapshot(roster, pdMS_TO_TICKS(20))) {
        for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
            if (roster.entries[i].node_id != NODE_ROSTER_EMPTY_ID) nodes++;
        }
    }
    snprintf(value, sizeof(value), "%lu", (unsigned long)radioPacketCount());
    snprintf(sub, sizeof(sub), "%u node%s", (unsigned)nodes, nodes == 1 ? "" : "s");
    statCard(statCardX(1), cy, STAT_CARD_W, "DECODED", COL_GOOD, value, sub);

    snprintf(value, sizeof(value), "%lums", (unsigned long)estimateTimeOnAirMs(ch.sf, ch.bw_khz));
    snprintf(sub, sizeof(sub), "CR4/%u 0x%02X", (unsigned)ch.cr_denom, (unsigned)ch.sync_word);
    statCard(statCardX(2), cy, STAT_CARD_W, "AIRTIME", COL_DIM, value, sub);
}

// Satellites used, sampled every 2s into 30 buckets — a 60s window for 30
// bytes of static RAM, the same shape and budget as Activity's packet ring.
// Sampled here rather than in gps_task because it is a display concern: the
// task already has the fix, this only remembers it.
//
// A fix hunting under tree cover while logging continues is the failure that
// quietly ruins a wardrive, and no instantaneous readout can show it — which
// is the whole reason this card carries a time series (option B,
// docs/research/2026-09-06-og-card-redesigns.html).
constexpr uint8_t SAT_RING_LEN = 30;
constexpr uint32_t SAT_SAMPLE_MS = 2000;
uint8_t satRing[SAT_RING_LEN] = {};
uint8_t satRingNext = 0;
uint8_t satRingCount = 0;
uint32_t satRingLastMs = 0;

void sampleSatCount(const GpsFix &fix, bool have) {
    const uint32_t now = millis();
    if (satRingLastMs != 0 && now - satRingLastMs < SAT_SAMPLE_MS) return;
    satRing[satRingNext] = (have && fix.has_position) ? fix.satellites : 0;
    satRingNext = (uint8_t)((satRingNext + 1) % SAT_RING_LEN);
    if (satRingCount < SAT_RING_LEN) satRingCount++;
    satRingLastMs = now;
}

// GPS, view 1 of 2 (Cell is view 2). Redesigned 2026-09-06 into the shared
// band-plus-cards language. The wardriving question is not "do I have a fix"
// but "are my detections getting positions" — a detection logged without one
// is a wasted data point, and nothing on the device said so before.
void drawGpsPage() {
    GpsFix fix;
    const bool have = gpsGetFix(fix, pdMS_TO_TICKS(100));
    sampleSatCount(fix, have);

    constexpr int16_t PX = 2, PW = 236, PH = 43;
    const int16_t py = HEADER_H + 2;
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(PX + 4, py + 3);
    uiTft->print("SATS USED  60s");
    drawPlotWell(HEADER_H, py + PH);

    // Dropouts within the window, and how long ago the most recent one was.
    uint8_t dropouts = 0;
    uint32_t lastDropAgoS = 0;
    bool prevHadFix = true;
    for (uint8_t i = 0; i < satRingCount; i++) {
        const uint8_t idx = (uint8_t)((satRingNext + SAT_RING_LEN - satRingCount + i) % SAT_RING_LEN);
        const bool hasFix = satRing[idx] > 0;
        if (prevHadFix && !hasFix) {
            dropouts++;
            lastDropAgoS = (uint32_t)(satRingCount - i) * SAT_SAMPLE_MS / 1000;
        }
        prevHadFix = hasFix;
    }
    const char *verdict = dropouts == 0 ? "STABLE" : "HUNTING";
    uiTft->setTextColor(dropouts == 0 ? COL_GOOD : COL_WARN, COL_BG);
    uiTft->setCursor(PX + PW - 4 - (int16_t)strlen(verdict) * 6, py + 3);
    uiTft->print(verdict);

    // Bars, not a step line (operator request, 2026-09-06, after seeing both on
    // hardware). The earlier reasoning was that satellites-used is a level and a
    // gap in a line reads as loss — true, but at a 7px pitch on a 240px panel a
    // 1px line is simply harder to read than a filled bar, and the red floor
    // carries the dropout just as clearly. Bars start 2px under the label row
    // and run to the well floor, so they use the full height available.
    const int16_t bw = (PW - 8) / SAT_RING_LEN;
    const int16_t base = py + PH - 1; // the well floor doubles as the baseline
    const int16_t top = py + 13;      // 2px clear of the "SATS USED 60s" row
    const int16_t plotH = (int16_t)(base - top);
    constexpr uint8_t SAT_FULL = 12;  // a comfortable 3D fix; above this bars top out

    for (uint8_t i = 0; i < satRingCount; i++) {
        const uint8_t idx = (uint8_t)((satRingNext + SAT_RING_LEN - satRingCount + i) % SAT_RING_LEN);
        const uint8_t v = satRing[idx];
        const int16_t bx = PX + 4 + i * bw;
        if (v == 0) {
            // No fix: a solid floor segment, not an absent bar. Absence of ink
            // would read as "no data yet" — this is data, and it says the run
            // was unmappable for those two seconds.
            uiTft->fillRect(bx, base - 1, bw - 1, 2, COL_BAD);
            continue;
        }
        int16_t h = (int16_t)(plotH * (v > SAT_FULL ? SAT_FULL : v) / SAT_FULL);
        if (h < 1) h = 1;
        uiTft->fillRect(bx, base - h + 1, bw - 1, h, v < 5 ? COL_WARN : COL_GOOD);
    }

    const int16_t cy = py + PH + 4;
    char value[14], sub[16];

    if (have && fix.has_position) {
        snprintf(value, sizeof(value), "%s", fix.fix_type >= 3 ? "3D" : "2D");
        snprintf(sub, sizeof(sub), "%u of %u", (unsigned)fix.satellites, (unsigned)fix.sats_in_view);
        statCard(statCardX(0), cy, STAT_CARD_W, "FIX", COL_GOOD, value, sub, COL_GOOD);
    } else {
        // Before a fix, sats-IN-VIEW is the number that matters: it says
        // whether the antenna can see sky at all, minutes before a fix lands.
        snprintf(value, sizeof(value), "NONE");
        snprintf(sub, sizeof(sub), fix.sats_in_view > 0 ? "%u in view" : "no sky", (unsigned)fix.sats_in_view);
        statCard(statCardX(0), cy, STAT_CARD_W, "FIX",
                 fix.sats_in_view > 0 ? COL_WARN : COL_BAD, value, sub,
                 fix.sats_in_view > 0 ? COL_WARN : COL_BAD);
    }

    // Against detections seen, not rows written: loggerRowsUntagged() counts
    // at batch-accept, so an SD outage would otherwise make this read as if
    // positions had been lost when it was storage that failed.
    const uint32_t seen = radioPacketCount();
    const uint32_t untagged = loggerRowsUntagged();
    const uint32_t tagged = seen > untagged ? seen - untagged : 0;
    snprintf(value, sizeof(value), "%lu", (unsigned long)tagged);
    snprintf(sub, sizeof(sub), "%lu untagged", (unsigned long)untagged);
    statCard(statCardX(1), cy, STAT_CARD_W, "TAGGED", untagged == 0 ? COL_GOOD : COL_WARN, value, sub);

    if (have && fix.has_time) {
        snprintf(value, sizeof(value), "%02u:%02u", (unsigned)fix.hour, (unsigned)fix.minute);
        snprintf(sub, sizeof(sub), "UTC  q%u", (unsigned)fix.fix_quality);
    } else {
        snprintf(value, sizeof(value), "--:--");
        snprintf(sub, sizeof(sub), "nmea %lu", (unsigned long)gpsSentenceCount());
    }
    statCard(statCardX(2), cy, STAT_CARD_W, "UTC", COL_DIM, value, sub);

    // Dropout detail replaces the UTC card's subtitle only when there is one
    // to report — it is the actionable half of the band above.
    if (dropouts > 0) {
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_WARN, COL_BG);
        uiTft->setCursor(statCardX(2) + 4, cy + 44);
        snprintf(sub, sizeof(sub), "%u drop %lus", (unsigned)dropouts, (unsigned long)lastDropAgoS);
        uiTft->print(sub);
    }
}

// Outline + proportional fill, same visual language as drawBattery() —
// turns "312k heap" into something scannable instead of a number to
// compare against 512 in your head. ~512KB is the ESP32-S3FN8's total SRAM
// with no PSRAM (docs/DESIGN.md §1).
//

// Free heap and battery, sampled once a minute into 30 buckets — a 30-minute
// window for 60 bytes of static RAM. Heap is stored in 2KB units so a uint8
// spans the ESP32-S3FN8's whole ~512KB SRAM (docs/DESIGN.md S1) without a
// wider type.
//
// System's question is "will the device survive the drive", which is a
// question about a trend: every value on the old card was an instant, so a
// slow leak and a healthy idle looked identical until one of them wasn't
// (option B, docs/research/2026-09-06-og-card-redesigns.html).
constexpr uint8_t SYS_RING_LEN = 30;
constexpr uint32_t SYS_SAMPLE_MS = 60000;
uint8_t heapRing[SYS_RING_LEN] = {};
uint8_t battRing[SYS_RING_LEN] = {};
uint8_t sysRingNext = 0;
uint8_t sysRingCount = 0;
uint32_t sysRingLastMs = 0;

void sampleSystemTrend() {
    const uint32_t now = millis();
    if (sysRingLastMs != 0 && now - sysRingLastMs < SYS_SAMPLE_MS) return;
    const uint32_t heapK = ESP.getFreeHeap() / 1024;
    heapRing[sysRingNext] = (uint8_t)(heapK / 2 > 255 ? 255 : heapK / 2);
    battRing[sysRingNext] = batteryPercent();
    sysRingNext = (uint8_t)((sysRingNext + 1) % SYS_RING_LEN);
    if (sysRingCount < SYS_RING_LEN) sysRingCount++;
    sysRingLastMs = now;
}

uint8_t sysRingAt(const uint8_t *ring, uint8_t i) {
    return ring[(uint8_t)((sysRingNext + SYS_RING_LEN - sysRingCount + i) % SYS_RING_LEN)];
}

// SYSTEM, the one single-view card. Redesigned 2026-09-06: a 30-minute free-heap
// trace as the band, then battery / heap / uptime cards. Keys, health-row count
// and firmware version keep their bottom line — lower-priority context that
// still has to be readable before driving off with the lid shut.
void drawSystemPage() {
    sampleSystemTrend();
    char buf[16], value[14], sub[16];

    constexpr int16_t PX = 2, PW = 236, PH = 43;
    const int16_t py = HEADER_H + 2;
    drawPlotWell(HEADER_H, py + PH);

    // Verdict from the window's own endpoints. Below two samples there is no
    // trend to report, and saying "FLAT" then would be an unearned claim. It
    // rides the heap lane's readout now rather than taking a header row of its
    // own — it is a statement about the heap, not about the card.
    const char *verdict = "--";
    uint16_t verdictCol = COL_DIM;
    if (sysRingCount >= 2) {
        const int16_t firstK = (int16_t)sysRingAt(heapRing, 0) * 2;
        const int16_t lastK = (int16_t)sysRingAt(heapRing, (uint8_t)(sysRingCount - 1)) * 2;
        const int16_t deltaK = (int16_t)(lastK - firstK);
        if (deltaK <= -16) { verdict = "FALLING"; verdictCol = COL_BAD; }
        else if (deltaK <= -6) { verdict = "DRIFT"; verdictCol = COL_WARN; }
        else { verdict = "FLAT"; verdictCol = COL_GOOD; }
    }
    // A dead keyboard outranks a heap trend, so it takes the readout outright —
    // it is the one thing here an operator must not miss.
    if (!keyboardReady) {
        verdict = "NO KEYS";
        verdictCol = COL_BAD;
    }

    // Two stacked lanes, not two traces overlaid (operator report, 2026-09-06:
    // "the line doesn't really pan out or read very well"). Heap and battery
    // share nothing but a time axis, so drawing them in one plot meant a white
    // line wandering across a green area at an unrelated scale — the classic
    // dual-axis mistake, and unreadable at 45px. Small multiples instead: each
    // series gets its own lane, own baseline and own label, and the only thing
    // being compared is shape against shape.
    //
    // Each lane auto-scales to its own window but with a MINIMUM span, which is
    // the other half of why the old plot read as noise: pure min/max scaling
    // amplifies a 2KB wobble to full height, so a perfectly flat heap looked
    // alarming. With a floor on the span, flat reads flat and only real
    // movement fills the lane.
    constexpr int16_t LANE_H = 19;
    const int16_t lane1Y = HEADER_H + 3, lane2Y = HEADER_H + 25;

    auto drawLane = [&](int16_t y, const uint8_t *ring, uint8_t minSpan, uint16_t colour,
                        const char *label, const char *readout, uint16_t readoutCol) {
        uint8_t lo = 255, hi = 0;
        for (uint8_t i = 0; i < sysRingCount; i++) {
            const uint8_t v = sysRingAt(ring, i);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (sysRingCount == 0) { lo = 0; hi = minSpan; }
        uint8_t span = (uint8_t)(hi - lo);
        if (span < minSpan) {
            // Centre the flat window inside the minimum span so a steady value
            // sits mid-lane rather than pinned to the floor.
            const uint8_t pad = (uint8_t)((minSpan - span) / 2);
            lo = (uint8_t)(lo > pad ? lo - pad : 0);
            span = minSpan;
        }
        const int16_t bw = (PW - 8) / SYS_RING_LEN;
        const int16_t base = y + LANE_H - 1;
        for (uint8_t i = 0; i < sysRingCount; i++) {
            const uint8_t v = sysRingAt(ring, i);
            const int16_t rel = (int16_t)(v > lo ? v - lo : 0);
            int16_t h = (int16_t)((LANE_H - 2) * rel / span) + 1;
            if (h > LANE_H) h = LANE_H;
            const int16_t bx = PX + 4 + i * bw;
            uiTft->fillRect(bx, base - h + 1, bw - 1, h, colour);
        }
        // Label and current value punch through the plot with an opaque
        // background, so neither has to reserve empty space above the lane.
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(PX + 4, y);
        uiTft->print(label);
        uiTft->setTextColor(readoutCol, COL_BG);
        uiTft->setCursor(234 - (int16_t)strlen(readout) * 6, y);
        uiTft->print(readout);
    };

    char readout[20];
    snprintf(readout, sizeof(readout), "%luk %s", (unsigned long)(ESP.getFreeHeap() / 1024), verdict);
    drawLane(lane1Y, heapRing, 8, COL_GOOD, "HEAP", readout, verdictCol);

    const uint8_t battPct = batteryPercent();
    snprintf(readout, sizeof(readout), "%u%%", (unsigned)battPct);
    drawLane(lane2Y, battRing, 5, COL_DIM, "BATT", readout,
             battPct > 20 ? COL_FG : COL_BAD);

    const int16_t cy = py + PH + 4;
    const uint32_t mv = batteryMilliVolts();

    // The subtitle is the MEASURED discharge rate over a stated window, not a
    // projected runtime. V2_DESIGN.md S3 forbids presenting derived confidence
    // as measurement, and "~4h left" is a projection dressed as a reading —
    // this states the slope and the span it came from and lets the operator do
    // the division.
    if (mv == 0) {
        snprintf(value, sizeof(value), "--");
        snprintf(sub, sizeof(sub), "no reading");
        statCard(statCardX(0), cy, STAT_CARD_W, "BATTERY", COL_DIM, value, sub);
    } else {
        const uint8_t pct = batteryPercent();
        snprintf(value, sizeof(value), "%u%%", (unsigned)pct);
        const uint32_t spanMin = (uint32_t)(sysRingCount - 1) * (SYS_SAMPLE_MS / 60000);
        if (sysRingCount >= 5 && spanMin > 0) {
            const int16_t drop = (int16_t)sysRingAt(battRing, 0) -
                                 (int16_t)sysRingAt(battRing, (uint8_t)(sysRingCount - 1));
            snprintf(sub, sizeof(sub), "%+d%%/h %lum", -(int)(drop * 60 / (int)spanMin),
                     (unsigned long)spanMin);
        } else {
            snprintf(sub, sizeof(sub), "%.2fV", mv / 1000.0);
        }
        statCard(statCardX(0), cy, STAT_CARD_W, "BATTERY", pct > 20 ? COL_GOOD : COL_BAD, value, sub,
                 pct > 20 ? COL_FG : COL_BAD);
    }

    const uint32_t heap = ESP.getFreeHeap();
    snprintf(value, sizeof(value), "%luk", (unsigned long)(heap / 1024));
    snprintf(sub, sizeof(sub), "min %luk", (unsigned long)(ESP.getMinFreeHeap() / 1024));
    statCard(statCardX(1), cy, STAT_CARD_W, "HEAP", heapUsageColour(heap / 1024), value, sub,
             heapUsageColour(heap / 1024));

    // Health rows keep their home here rather than on a bottom line: they
    // confirm session.csv is actually being written, which is a real check to
    // make before driving off with the lid shut.
    snprintf(value, sizeof(value), "%lum", (unsigned long)(millis() / 60000));
    snprintf(sub, sizeof(sub), "health %lu", (unsigned long)loggerSessionRows());
    statCard(statCardX(2), cy, STAT_CARD_W, "UPTIME", COL_DIM, value, sub);
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
// on MenuAction rather than one function per list.
//
// The per-tool live-status cases this used to carry went with the Tools and
// Analyze groups (2026-09-06): every one of those tools now reports its own
// state on the card view that owns it, which has a whole panel to do it in
// rather than a value column.
const char *menuEntryValue(MenuAction action) {
    switch (action) {
        case MenuAction::SELECT_MESHTASTIC:
            return radioActiveProfile() == MissionProfile::MESHTASTIC ? "ACTIVE" : "";
        case MenuAction::SELECT_MESHCORE:
            return radioActiveProfile() == MissionProfile::MESHCORE ? "ACTIVE" : "";
        case MenuAction::WIFI_TOGGLE: return wifiIsEnabled() ? "ON" : "OFF";
        // No value: the key belongs in the toast the row fires, not on a
        // list that is on screen whenever the menu is open.
        case MenuAction::WIFI_KEY_SHOW: return "";
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
// Rolling packets/second for the Activity sparkline: 30 one-second buckets,
// 30 bytes static, no heap -- the budget docs/UI-Recommendations.html sets for
// this feature. Sampled here rather than in radio_task because it is a display
// concern: the radio already counts packets, this only differences that count
// once a second.
constexpr uint8_t PKT_RING_LEN = 30;
uint8_t pktRing[PKT_RING_LEN] = {};
uint8_t pktRingNext = 0;
uint8_t pktRingCount = 0;
uint32_t pktRingLastMs = 0;
uint32_t pktRingLastTotal = 0;

void samplePacketRate() {
    const uint32_t now = millis();
    if (pktRingLastMs != 0 && now - pktRingLastMs < 1000) return;
    const uint32_t total = radioPacketCount();
    if (pktRingLastMs != 0) {
        const uint32_t delta = total - pktRingLastTotal;
        pktRing[pktRingNext] = delta > 255 ? 255 : (uint8_t)delta;
        pktRingNext = (uint8_t)((pktRingNext + 1) % PKT_RING_LEN);
        if (pktRingCount < PKT_RING_LEN) pktRingCount++;
    }
    pktRingLastMs = now;
    pktRingLastTotal = total;
}

void drawActivitySummary() {
    samplePacketRate();

    // Sparkline: packets/sec over the last 30s. The busiest bucket is drawn in
    // warn so a burst is findable at a glance without a Y axis, which will not
    // fit at this size.
    //
    // No "INGRESS (30s)" / "PKTS/S" caption: the guide had one, but the plot
    // is self-evident and the row it occupied is better spent on plot height
    // at 240x135, where vertical space is the scarce axis.
    constexpr int16_t PX = 2, PW = 236, PH = 43;
    const int16_t py = HEADER_H + 2;
    drawPlotWell(HEADER_H, py + PH);
    uint8_t peak = 1;
    for (uint8_t i = 0; i < pktRingCount; i++) {
        if (pktRing[i] > peak) peak = pktRing[i];
    }
    const int16_t bw = (PW - 4) / PKT_RING_LEN;
    for (uint8_t i = 0; i < pktRingCount; i++) {
        // Oldest on the left: walk back from the write cursor.
        const uint8_t idx = (uint8_t)((pktRingNext + PKT_RING_LEN - pktRingCount + i) % PKT_RING_LEN);
        const uint8_t v = pktRing[idx];
        if (v == 0) continue;
        const int16_t h = (int16_t)((PH - 4) * v / peak);
        const int16_t bx = PX + 2 + i * bw;
        uiTft->fillRect(bx, py + PH - h, bw - 1, h, v == peak ? COL_WARN : COL_GOOD);
    }

    // Three triage cards.
    const int16_t cy = py + PH + 4;
    char value[14], sub[16];

    const EnergyStrongestPeak strongest = radioEnergyStrongestPeak();
    if (strongest.valid) {
        snprintf(value, sizeof(value), "%.1f", (double)strongest.freq_mhz);
        snprintf(sub, sizeof(sub), "%.0f dBm", (double)strongest.rssi_peak_dbm_x10 / 10.0);
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(sub, sizeof(sub), "no sweep");
    }
    statCard(statCardX(0), cy, STAT_CARD_W, "SWEEP PK", COL_WARN, value, sub);

    // Last decoded packet. RSSI and SNR are real here, which is why this card
    // replaced the proposal's "CAD HIT ... +8.5dB SNR": CAD returns a binary
    // detected/free/timeout from the modem and yields no SNR at all.
    static CaptureHistory history;
    CaptureSummary latest;
    bool havePacket = analyzerCaptureHistorySnapshot(history) &&
                      captureHistoryEntryAt(history, 0, latest);
    if (havePacket) {
        snprintf(value, sizeof(value), "%.0f", (double)latest.rssi_dbm);
        snprintf(sub, sizeof(sub), "%+.1fdB SNR", (double)latest.snr_db);
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(sub, sizeof(sub), "no packets");
    }
    statCard(statCardX(1), cy, STAT_CARD_W, "LAST PKT", COL_GOOD, value, sub);

    // Away time: the most recent bounded action to have held the radio, shown
    // plainly. No budget line -- §6.3 measured the cost and deliberately left
    // the policy to an operator decision, so a limit here would invent one.
    struct { uint32_t ms; const char *who; } aways[] = {
        {radioEnergyLastAwayMs(), "sweep"}, {radioFocusLastAwayMs(), "focus"},
        {radioCellLastAwayMs(), "cell"},    {radioScopeLastAwayMs(), "scope"},
        {radioDiscoveryLastAwayMs(), "probe"},
    };
    uint32_t awayMs = 0; const char *awayWho = "none yet";
    for (const auto &a : aways) {
        if (a.ms > awayMs) { awayMs = a.ms; awayWho = a.who; }
    }
    if (awayMs > 0) {
        snprintf(value, sizeof(value), "%.1fs", awayMs / 1000.0);
        snprintf(sub, sizeof(sub), "last: %s", awayWho);
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(sub, sizeof(sub), "none yet");
    }
    statCard(statCardX(2), cy, STAT_CARD_W, "AWAY T", COL_DIM, value, sub);
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
// drawFreqBar()/drawMeterBar() rather than a third bar style.
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

// Meter, in the card idiom (2026-09-06). It is not a bounded action, so it
// takes the well-plus-three-cards shape the carousel cards use rather than
// Probe/Sweep/Cell/Focus's result skeleton: the gauge is the band, and the
// context that used to be four stacked text lines plus a right column becomes
// three cards.
//
// The two source branches (a live Scope sample vs. the newest decoded packet)
// previously duplicated the whole layout between them. They now differ only in
// what they put in the cards, which is the only thing that actually differs.
void drawMeterPage() {
    static ScopeTrace trace;
    const bool haveTrace = radioScopeTraceSnapshot(trace, 0) && trace.count > 0;
    int8_t scopeSample = 0;
    const bool haveScopeSample = haveTrace && scopeTraceSampleAt(trace, 0, scopeSample);

    static CaptureHistory captures;
    CaptureSummary latest;
    const bool haveCapture = analyzerCaptureHistorySnapshot(captures, pdMS_TO_TICKS(50)) &&
                             captureHistoryEntryAt(captures, 0, latest);

    // Scope wins when it is actively running, when nothing else exists, or when
    // its capture is newer than the latest packet -- "current scope RSSI", not
    // a stale one left over from an earlier visit.
    const bool useScope = haveScopeSample &&
        (radioScopeAcquireIsActive() || !haveCapture || trace.start_millis >= latest.rx_millis);

    if (!useScope && !haveCapture) {
        drawEmptyView("NO MEASUREMENT", "fills while Watch runs");
        return;
    }

    const float dbm = useScope ? (float)scopeSample : latest.rssi_dbm;
    const float freq = useScope ? trace.tuned_freq_mhz : latest.freq_mhz;
    const uint32_t ageS = (millis() - (useScope ? trace.start_millis : latest.rx_millis)) / 1000;

    // Band: the gauge, in a well like the carousel plots. Range is -120..0, not
    // Scope's -120..-30: real readings from a close repeater hit -16dBm and
    // clipped flat against the old ceiling (operator, 2026-09-04). Scope's own
    // ceiling exists so trace heights stay comparable across captures, which is
    // a different job.
    constexpr float DISPLAY_LO = -120.0f, DISPLAY_HI = 0.0f;
    const int16_t py = HEADER_H + 2;
    drawPlotWell(HEADER_H, py + 43);

    char buf[24];
    uiTft->setTextSize(2);
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(4, py + 4);
    snprintf(buf, sizeof(buf), "%d dBm", (int)dbm);
    uiTft->print(buf);

    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_DIM, COL_BG);
    snprintf(buf, sizeof(buf), "%.3f MHz", (double)freq);
    uiTft->setCursor(234 - (int16_t)strlen(buf) * 6, py + 8);
    uiTft->print(buf);

    drawMeterBar(4, py + 24, 232, 12, dbm, DISPLAY_LO, DISPLAY_HI);

    const int16_t cy = py + 43 + 4;
    char value[14], sub[16];

    snprintf(value, sizeof(value), "%s", useScope ? "SCOPE" : "WATCH");
    if (useScope && radioScopeAcquireIsActive()) {
        snprintf(sub, sizeof(sub), "Watch away");
    } else {
        snprintf(sub, sizeof(sub), "%lus ago", (unsigned long)ageS);
    }
    statCard(statCardX(0), cy, STAT_CARD_W, "SOURCE", useScope ? COL_WARN : COL_GOOD, value, sub,
             useScope ? COL_WARN : COL_GOOD);

    // Scope samples raw RSSI at one frequency without demodulating anything, so
    // it genuinely has no SNR to report -- an honest omission, the same rule as
    // Waterfall's "no fabricated vertical texture" (docs/research/V2_DESIGN.md).
    if (useScope) {
        snprintf(value, sizeof(value), "--");
        snprintf(sub, sizeof(sub), "no decode");
        statCard(statCardX(1), cy, STAT_CARD_W, "SNR", COL_DIM, value, sub);
        snprintf(value, sizeof(value), "%ums", (unsigned)trace.sample_interval_ms);
        snprintf(sub, sizeof(sub), "per sample");
        statCard(statCardX(2), cy, STAT_CARD_W, "RATE", COL_DIM, value, sub);
    } else {
        snprintf(value, sizeof(value), "%+.1f", (double)latest.snr_db);
        snprintf(sub, sizeof(sub), "dB");
        statCard(statCardX(1), cy, STAT_CARD_W, "SNR", COL_GOOD, value, sub);
        snprintf(value, sizeof(value), "SF%u", (unsigned)latest.sf);
        snprintf(sub, sizeof(sub), "BW%.0f CR4/%u", (double)latest.bw_khz_x10 / 10.0,
                 (unsigned)latest.cr_denom);
        statCard(statCardX(2), cy, STAT_CARD_W, "PARAMS", COL_DIM, value, sub);
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
        // "NO HISTORY", not a second "NO SWEEPS YET": the Sweep view one press
        // up already says that and already carries the key hint. This view is
        // downstream of it, so it says what it is waiting on instead of
        // repeating the instruction. Its old hint ("Enter: start repeat
        // Sweep") is also what proved hand-typed hints drift — Enter became
        // single-shot and R became repeat, and the string did not follow.
        drawEmptyView("NO HISTORY", "fills as sweeps complete");
        return;
    }

    // First pass: total hit-bin count across every stored row, not just
    // whatever fits in the box below — the same "what did we find" callout
    // shape drawSweepPage()/drawCellPage() already use (peaks / strongest
    // signal), for a consistent headline across all three pages.
    uint32_t totalHits = 0;
    uint32_t totalCaptures = 0;
    uint16_t latestBinCount = 0;
    static WaterfallRow row;
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
    // R toggles it (cardRepeatAction()). Size 1, not size 2
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
    static uint8_t columns[PLOT_W];
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
    static ScopeTrace trace;
    const bool have = radioScopeTraceSnapshot(trace, pdMS_TO_TICKS(50));
    const bool running = radioScopeAcquireIsActive();
    const bool holdExpired = !running && scopeTerminalShownAt != 0 &&
                             millis() - scopeTerminalShownAt >= RESULT_HOLD_MS;

    if (!have || trace.count == 0) {
        drawEmptyView("NO SCOPE YET", running ? "Watch away" : cardHintLine(UiPage::SCOPE));
        return;
    }

    // Frequency is the hero, not the state word: what this trace is *of* is the
    // durable fact, where CAPTURING/CAPTURED is momentary. The age takes the
    // top right (operator request, 2026-09-06) — once a trace is on screen,
    // "CAPTURED" is self-evident and how old it is is not. While running the
    // slot carries the live state instead, which is the one time it is news.
    char freqBuf[28], status[16];
    snprintf(freqBuf, sizeof(freqBuf), "%.3f MHz", (double)trace.tuned_freq_mhz);
    if (running) {
        snprintf(status, sizeof(status), "CAPTURING");
    } else {
        snprintf(status, sizeof(status), "%s", resultAge(scopeTerminalShownAt));
    }
    drawActionHero(freqBuf, nullptr, COL_DIM, status,
                   running ? COL_WARN : (holdExpired ? COL_DIM : COL_GOOD));

    uiTft->setTextSize(1);
    uiTft->setTextColor(running ? COL_WARN : COL_DIM, COL_BG);
    uiTft->setCursor(2, HEADER_H + 24);
    if (running) {
        uiTft->print("Watch away");
    } else {
        char detail[28];
        snprintf(detail, sizeof(detail), "%u samples  %ums each", (unsigned)trace.count,
                 (unsigned)trace.sample_interval_ms);
        uiTft->print(detail);
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
// Captures inspector (V2 UI slice, docs/UI-Recommendations.html P4). Shows
// the RF and framing detail behind one capture, plus the retained frame
// prefix, which the amended ROADMAP boundary now permits on-device.
//
// Two fields from the proposal are absent because the firmware cannot source
// them honestly. "CRC: OK (valid)" is tautological -- a frame that failed CRC
// never becomes a Detection, so every entry in this ring is valid by
// construction and printing it would be decoration. "AFC: +1.4kHz" is not
// captured at RX at all.
void drawCaptureInspector() {
    static CaptureHistory history;
    CaptureSummary cap;
    const uint8_t idx = captureInspectIndex();
    if (!analyzerCaptureHistorySnapshot(history, pdMS_TO_TICKS(50)) ||
        !captureHistoryEntryAt(history, idx, cap)) {
        uiTft->setTextSize(1);
        uiTft->setTextColor(COL_DIM, COL_BG);
        uiTft->setCursor(2, HEADER_H + 8);
        uiTft->print("capture no longer in ring");
        return;
    }

    char line[48];
    // Banner: which entry of how many, so browsing has a position.
    uiTft->fillRect(2, HEADER_H + 2, 236, 12, COL_DIM);
    uiTft->setTextSize(1);
    uiTft->setTextColor(COL_BG, COL_DIM);
    uiTft->setCursor(4, HEADER_H + 5);
    if (cap.node_id != 0) {
        snprintf(line, sizeof(line), "INSPECT !%08lx  %u/%u",
                 (unsigned long)cap.node_id, (unsigned)(idx + 1), (unsigned)history.count);
    } else {
        snprintf(line, sizeof(line), "INSPECT unknown id  %u/%u",
                 (unsigned)(idx + 1), (unsigned)history.count);
    }
    uiTft->print(line);

    const int16_t y1 = HEADER_H + 19;
    uiTft->setTextColor(COL_FG, COL_BG);
    uiTft->setCursor(2, y1);
    snprintf(line, sizeof(line), "RSSI: %.0f dBm", (double)cap.rssi_dbm);
    uiTft->print(line);
    uiTft->setTextColor(COL_GOOD, COL_BG);
    uiTft->setCursor(126, y1);
    snprintf(line, sizeof(line), "SNR: %+.1f dB", (double)cap.snr_db);
    uiTft->print(line);

    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(2, y1 + 11);
    snprintf(line, sizeof(line), "FREQ: %.3f", (double)cap.freq_mhz);
    uiTft->print(line);
    uiTft->setCursor(126, y1 + 11);
    snprintf(line, sizeof(line), "LEN: %u bytes", (unsigned)cap.raw_len);
    uiTft->print(line);

    uiTft->setCursor(2, y1 + 22);
    snprintf(line, sizeof(line), "MOD: SF%u/BW%.0f/CR4:%u", (unsigned)cap.sf,
             (double)cap.bw_khz_x10 / 10.0, (unsigned)cap.cr_denom);
    uiTft->print(line);

    // Raw prefix. The header says how much of the frame this is, so a 32-byte
    // window onto a longer packet is never mistaken for the whole thing.
    const int16_t boxY = y1 + 34;
    uiTft->drawRect(2, boxY, 236, 48, COL_DIM);
    uiTft->setTextColor(COL_DIM, COL_BG);
    uiTft->setCursor(4, boxY + 3);
    if (cap.raw_len > cap.raw_prefix_len) {
        snprintf(line, sizeof(line), "RAW OTA BYTES (first %u of %u):",
                 (unsigned)cap.raw_prefix_len, (unsigned)cap.raw_len);
    } else {
        snprintf(line, sizeof(line), "RAW OTA BYTES (%u):", (unsigned)cap.raw_prefix_len);
    }
    uiTft->print(line);

    // 12 bytes per row, not 16: each byte prints as "FF " which is 18px at
    // size 1, so 16 would need 288px on a 240px panel and wrap onto itself.
    // 32 bytes therefore takes three rows, which is what the removed
    // "up/down: browse" hint line was occupying -- the keys are the same as
    // every other browsable view, so the space is better spent on the bytes.
    constexpr uint8_t HEX_PER_ROW = 12;
    uiTft->setTextColor(COL_WARN, COL_BG);
    for (uint8_t row = 0; row < 3; row++) {
        const uint8_t first = (uint8_t)(row * HEX_PER_ROW);
        if (first >= cap.raw_prefix_len) break;
        char hex[40];
        int n = 0;
        for (uint8_t i = first;
             i < cap.raw_prefix_len && i < (uint8_t)(first + HEX_PER_ROW); i++) {
            n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02X ", cap.raw_prefix[i]);
        }
        uiTft->setCursor(4, boxY + 14 + row * 11);
        uiTft->print(hex);
    }
}

void drawCapturesPage() {
    if (captureInspectIsOpen()) {
        drawCaptureInspector();
        return;
    }
    static CaptureHistory history;
    const bool have = analyzerCaptureHistorySnapshot(history, pdMS_TO_TICKS(50));
    if (!have || history.count == 0) {
        drawEmptyView("NO CAPTURES YET", "fills while Watch runs");
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
    static NodeRoster roster;
    if (!analyzerNodeRosterSnapshot(roster, pdMS_TO_TICKS(50))) {
        drawEmptyView("NO NODES YET", "fills while Watch runs");
        return;
    }

    uint8_t order[NODE_ROSTER_MAX_ENTRIES];
    uint8_t liveCount = 0;
    for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        if (roster.entries[i].node_id != NODE_ROSTER_EMPTY_ID) order[liveCount++] = i;
    }
    if (liveCount == 0) {
        drawEmptyView("NO NODES YET", "fills while Watch runs");
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
        uiTft->print(pageName(activeView()));
        // System names the build it is running. The header is where identity
        // lives, and this is the only card with room for it — the bottom line
        // that used to carry the version collided with the footer.
        if (activeView() == UiPage::SYSTEM) {
            uiTft->setTextColor(COL_DIM, COL_BG);
            uiTft->print("  ");
            uiTft->print(FIRMWARE_VERSION);
        }
    }

    drawBattery();

    // Status dot cluster: WiFi AP, GPS fix, heap health, RX activity — always
    // visible from any page instead of only their own. WiFi joined here
    // 2026-09-06 (operator request): it was text on System's bottom line, which
    // is a worse place for a binary state than the cluster that already exists
    // for exactly that. Client count moves to the web UI and the toggle toast;
    // a dot cannot carry it and does not need to.
    //
    // Cluster shifted left to 148 to fit a fourth dot: drawBattery() clears
    // from x=184, so 177 was the old right edge and there was no room after it.
    uiTft->fillCircle(148, 6, 2, wifiIsEnabled() ? COL_GOOD : COL_DIM);
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
        // The resolved card view, not `page` — a card renders whichever of its
        // views is current (ui_task.cpp's CARD_VIEWS), and every view is a real
        // UiPage with its own draw function, so this switch is unchanged apart
        // from what it switches on. A page opened directly from the menu
        // resolves to itself.
        switch (activeView()) {
            case UiPage::RADIO: drawRadioPage(); break;
            case UiPage::ACTIVITY: drawActivitySummary(); break;
            case UiPage::CHANNEL: drawChannelPage(); break;
            case UiPage::GPS: drawGpsPage(); break;
            case UiPage::SYSTEM: drawSystemPage(); break;
            case UiPage::PROBE: drawProbePage(); break;
            case UiPage::SWEEP: drawSweepPage(); break;
            case UiPage::CELL: drawCellPage(); break;
            case UiPage::FOCUS: drawFocusPage(); break;
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
