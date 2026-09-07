// LoRaTrace RX — ui_task lifecycle, input, and main loop.
//
// Split out of a single ~1265-line ui_task.cpp (2026-08-25 cleanup pass)
// into three files by concern: this file (task lifecycle, keyboard input,
// main loop, and all operator-facing state — page, menu, toast, RX pulse,
// brightness/idle-dim), drawing (ui_pages.cpp), and menu-action business
// logic (ui_actions.cpp). See ui_task.h for the subsystem design and
// ui_task_shared.h for the contract between these three files.

#include "ui_task.h"
#include "analyzer_state.h"
#include "ui_task_shared.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "backlight.h"
#include "board_pins.h"
#include "display_settings.h"
#include "sweep_margin_settings.h"
#include "keyboard.h"
#include "serial_control.h"
#include "serial_lock.h"
#include "memory_stats.h"
#include "radio_task.h"

// --- Shared state (extern-declared in ui_task_shared.h) ---
// Defined here, at plain file scope (not the anonymous namespace below) so
// it gets the external linkage ui_pages.cpp/ui_actions.cpp need.

// Draw target for every drawing function (ui_pages.cpp, this file's own
// fullRedraw()). Since Phase 6 points at an off-screen Arduino_Canvas_Indexed
// buffer, not the panel directly — see uiTaskStart() below for why.
Arduino_GFX *uiTft = nullptr;

// CRITICAL: the TCA8418 boots in SLEEP and reports nothing until
// explicitly configured, even with a healthy I2C bus — same failure shape
// as the GPS power rail. begin()+matrix() is the wake sequence, taken from
// bmorcelli/Launcher's confirmed-working Cardputer-ADV interface.
bool keyboardReady = false;

UiPage page = UiPage::RADIO;

// Toast layer: a brief overlay message for feedback not tied to whichever
// menu row is highlighted (e.g. confirming a toggle right before BACK
// leaves the menu). Static buffer, no heap allocation.
char toastMsg[48] = {0};
uint32_t toastShownAt = 0;

// See ui_task_shared.h — set below, the moment each result's own
// async-completion toast fires.
uint32_t probeTerminalShownAt = 0;
uint32_t sweepTerminalShownAt = 0;
uint32_t cellTerminalShownAt = 0;
uint32_t scopeTerminalShownAt = 0;

// activeBrightnessPercent is the operator's chosen level (5-100) — what
// idle-dim restores to on the next keypress, not necessarily what the
// backlight is driven at right now (see idleDimTargetPercent() below).
// Seeded from SD (uiTaskStart()'s `settings` param), so it survives a
// power cycle the same way channel overrides already do.
uint8_t activeBrightnessPercent = 100;
bool displayDimmed = false;

// Idle-dim timeout, cycled from System > Display's "Idle dim" row.
// Index 0 = Off (disables idle-dim); index 2 (60s) is the default.
const IdleTimeoutOption IDLE_TIMEOUT_OPTIONS[] = {
    {"Off", 0},
    {"30s", 30000},
    {"60s", 60000},
    {"2min", 120000},
    {"5min", 300000},
};
uint8_t idleTimeoutIndex = 2;

// Root menu table. "Profile" opens onto the real, technical profile names
// (Meshtastic/MeshCore; Reticulum/Spectrum join once they have a
// real Phase-9 sweep profile) instead of cycling one at a time on Enter.
// Deliberately not branded per-profile (docs/BRAND.md) — these are LoRa presets
// on one sniffer, not sibling products. Plain file scope (external
// linkage) because ui_pages.cpp's drawMenuList() identity-compares
// against it directly.
const MenuItem PROFILE_GROUP_ITEMS[] = {
    {"Meshtastic", ItemKind::ACTION, MenuAction::SELECT_MESHTASTIC, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"MeshCore", ItemKind::ACTION, MenuAction::SELECT_MESHCORE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Node IDs", ItemKind::ACTION, MenuAction::IDENTITY_CAPTURE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};

namespace {

// The off-screen buffer uiTft normally points at. Only null if the
// allocation in uiTaskStart() below failed, in which case uiTft falls back
// to the raw panel gfx pointer instead.
Arduino_Canvas_Indexed *canvas = nullptr;

// TCA8418 keyboard controller. Cardputer ADV replaced the base Cardputer's
// GPIO matrix with this I2C part — same SDA/SCL as the IO expander, a
// different address, ordinary shared-bus operation.
Adafruit_TCA8418 keys;

uint32_t lastPageChange = 0;

// Brightness/idle-dim live in their own nested group under System > Display
// (the first ui_menu.h nesting deeper than one level) rather than System's
// own flat list, on operator request. Brightness stays a SLIDER row.
constexpr MenuItem DISPLAY_GROUP_ITEMS[] = {
    {"Brightness", ItemKind::SLIDER, MenuAction::NONE, MenuAction::BRIGHTNESS_UP, MenuAction::BRIGHTNESS_DOWN, nullptr, 0},
    // Cycles Off/30s/60s/2min/5min on each Enter press, same "fires and
    // stays in the list" shape as WiFi/Debug, just cycling a value.
    {"Idle dim", ItemKind::ACTION, MenuAction::IDLE_TIMEOUT_CYCLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
// System's flat list grew to 5 rows (WiFi, Debug, Retry SD, Serial
// Control, Display) and started colliding with the footer's ",/. move"
// nav hint at the bottom of a 135px-tall panel -- drawMenuList() draws
// unconditionally at a fixed 24px/row with no overflow handling, so a
// 5th row's bottom edge (y=135) lands right on top of the footer text
// (y=126). Split into two more nested groups, the same "genuinely
// distinct topics" reasoning Display already used: Connectivity (the two
// external-interface toggles: WiFi AP, Serial Control) and Diagnostics
// (the two troubleshooting actions: Debug, SD). System itself drops to 3
// rows (2026-08-28 operator request). SD's row label is deliberately just
// "SD", not "Retry SD" -- its value already reads RETRY/READY
// (menuEntryValue(), ui_pages.cpp), so the row itself says "SD: RETRY" or
// "SD: READY" rather than "Retry SD: RETRY" (2026-08-28 operator request).
constexpr MenuItem CONNECTIVITY_GROUP_ITEMS[] = {
    {"WiFi", ItemKind::ACTION, MenuAction::WIFI_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"WiFi Key", ItemKind::INFO, MenuAction::WIFI_KEY_SHOW, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Serial Control", ItemKind::ACTION, MenuAction::SERIAL_CONTROL_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
constexpr MenuItem DIAGNOSTICS_GROUP_ITEMS[] = {
    {"Debug", ItemKind::ACTION, MenuAction::DEBUG_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"SD", ItemKind::ACTION, MenuAction::SD_RETRY, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
// Sweep's own tuning knobs, their own nested group under System (operator
// request 2026-09-03: adding a Margin slider alongside the existing
// Region cycle here — rather than as a 5th flat row on System's own list —
// keeps that list at its already-calibrated 4-row headroom; see
// CONNECTIVITY/DIAGNOSTICS_GROUP_ITEMS' own comment for the footer-
// collision math that headroom was set against). Named "Tuning", not
// "Sweep" (operator request, same day, after using it live) — the group
// sits alongside Sweep itself, the actual radio action, and reusing
// that name for a settings container read as two different things sharing
// one word. Same "category, not feature name" naming Display/
// Connectivity/Diagnostics already use. Margin is a SLIDER row, same shape
// as Display's Brightness — see energy_observation.h's
// ENERGY_SWEEP_MARGIN_MIN_DBM_X10/MAX/STEP for its bounds and
// docs/STATUS.md's "Sweep silence" investigation for why it became
// operator-adjustable.
constexpr MenuItem TUNING_GROUP_ITEMS[] = {
    {"Region", ItemKind::ACTION, MenuAction::REGION_CYCLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Margin", ItemKind::SLIDER, MenuAction::NONE, MenuAction::SWEEP_MARGIN_UP, MenuAction::SWEEP_MARGIN_DOWN, nullptr, 0},
    // Repeat Sweep's home-channel capture window (Off/1s/2s/4s) — the
    // survey-cadence-vs-packet-capture trade, see ui_menu.h's own comment
    // on CAPTURE_WINDOW_CYCLE.
    {"Capture", ItemKind::ACTION, MenuAction::CAPTURE_WINDOW_CYCLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
// Cross-check further down, once SYSTEM_GROUP_ITEMS exists, that its
// "Tuning" row's itemCount actually matches this array's real length.
constexpr MenuItem SYSTEM_GROUP_ITEMS[] = {
    {"Connectivity", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, CONNECTIVITY_GROUP_ITEMS, 3},
    {"Diagnostics", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, DIAGNOSTICS_GROUP_ITEMS, 2},
    {"Display", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, DISPLAY_GROUP_ITEMS, 2},
    {"Tuning", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, TUNING_GROUP_ITEMS, 3},
};
// A GROUP row's itemCount is hand-written and nothing at runtime notices
// when it drifts below its array's real length — the extra rows just
// silently never render. That exact bug shipped once (Region became
// System's 4th row while the count still said 3, docs/STATUS.md's Region
// entry) and was only caught by an operator not seeing the row on
// hardware. These make it a build error instead. One per GROUP row —
// including ROOT_ITEMS' own, below, which is where that historical bug
// actually was — so adding a row anywhere fails loudly rather than
// quietly.
template <size_t N>
constexpr size_t menuItemCount(const MenuItem (&)[N]) { return N; }
static_assert(SYSTEM_GROUP_ITEMS[0].itemCount == menuItemCount(CONNECTIVITY_GROUP_ITEMS),
              "System > Connectivity itemCount does not match CONNECTIVITY_GROUP_ITEMS");
static_assert(SYSTEM_GROUP_ITEMS[1].itemCount == menuItemCount(DIAGNOSTICS_GROUP_ITEMS),
              "System > Diagnostics itemCount does not match DIAGNOSTICS_GROUP_ITEMS");
static_assert(SYSTEM_GROUP_ITEMS[2].itemCount == menuItemCount(DISPLAY_GROUP_ITEMS),
              "System > Display itemCount does not match DISPLAY_GROUP_ITEMS");
static_assert(SYSTEM_GROUP_ITEMS[3].itemCount == menuItemCount(TUNING_GROUP_ITEMS),
              "System > Tuning itemCount does not match TUNING_GROUP_ITEMS");
// Tools and Analyze are gone (2026-09-06). Both were pure navigation: every
// row opened a page that is now a view of the card that owns it (CARD_VIEWS
// below), reachable with up/down from the carousel, so the rows were a second
// road to a place you were already standing next to. That redundancy is the
// same "two separate navigation systems on screen" complaint that moved them
// out of hub pages and into the menu the day before — card views answered it
// properly, and the menu no longer has to.
//
// What is left is what a menu is actually for: things that change runtime
// behavior and have no card of their own. Trace returns to a root row, which
// is where it lived until Tools briefly adopted it — it is a Watch pause, not
// a page, and Enter on Radio is a binding, not a label. Keeping the named row
// is what CLAUDE.md's "new operator-facing behavior gets an on-device menu
// toggle" rule is asking for.
constexpr MenuItem ROOT_ITEMS[] = {
    {"Profile", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, PROFILE_GROUP_ITEMS, 3},
    {"Trace", ItemKind::ACTION, MenuAction::TRACE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"System", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 4},
};
constexpr uint8_t ROOT_COUNT = 3;
// ROOT_ITEMS' own GROUP rows need the same guard as SYSTEM_GROUP_ITEMS'
// above — and more so: the v0.8.9 bug those cite was *here*, on the row
// pointing at SYSTEM_GROUP_ITEMS, not inside it. Asserting only the
// children would have left the exact historical failure uncovered.
// ROOT_COUNT is hand-written for the same reason and drifts the same way.
static_assert(ROOT_ITEMS[0].itemCount == menuItemCount(PROFILE_GROUP_ITEMS),
              "root > Profile itemCount does not match PROFILE_GROUP_ITEMS");
static_assert(ROOT_ITEMS[2].itemCount == menuItemCount(SYSTEM_GROUP_ITEMS),
              "root > System itemCount does not match SYSTEM_GROUP_ITEMS");
static_assert(ROOT_COUNT == menuItemCount(ROOT_ITEMS),
              "ROOT_COUNT does not match ROOT_ITEMS");

// RX activity pulse: a brief, event-driven flash on the header's third
// status dot and a matching flash bar on RADIO, replacing an old idle
// heartbeat blink that only proved the UI task was alive, not that
// anything was being heard. Binary hold-then-revert, not an alpha-blended
// decay — RGB565 has no cheap alpha blending. File-local: only this file
// sets the deadline (on a new detection, in uiTask() below); ui_pages.cpp
// only reads rxPulseActive().
uint32_t rxPulseUntil = 0;
constexpr uint32_t RX_PULSE_MS = 220;

// Raw key-dump diagnostic, off by default and gated behind Serial Control
// (KEY_DUMP opcode). Exists because every key constant in keyboard.h was
// derived on paper and only ever checked by host tests asserting those same
// constants against themselves. Emits one line per raw FIFO event so a
// bench pass can settle the map instead of re-deriving it — this is what
// caught the Ctrl+S modifier chord dropping its own release event on real
// hardware (2026-08-30), which is why that chord was reverted in favor of
// a dedicated key (keyboard.h's KEY_RAW_R_PRESS).
bool keyDumpEnabled = false;

void keyDumpEmit(uint8_t rawEvent) {
    uint8_t row = 0;
    uint8_t col = 0;
    char line[96];
    if (keyboardPhysicalPosition(keyboardEventKeyNumber(rawEvent), row, col)) {
        snprintf(line, sizeof(line), "[keydump] raw=0x%02X K=%u %s row=%u col=%u t=%lu",
                 (unsigned)rawEvent, (unsigned)keyboardEventKeyNumber(rawEvent),
                 keyboardEventIsRelease(rawEvent) ? "UP" : "DN", (unsigned)row, (unsigned)col,
                 (unsigned long)millis());
    } else {
        snprintf(line, sizeof(line), "[keydump] raw=0x%02X K=%u %s row=? col=? t=%lu",
                 (unsigned)rawEvent, (unsigned)keyboardEventKeyNumber(rawEvent),
                 keyboardEventIsRelease(rawEvent) ? "UP" : "DN", (unsigned long)millis());
    }
    SerialLock lock(pdMS_TO_TICKS(10));
    if (lock.held()) serialPrintln(line);
}

// The level idle-dim actually drives: the lower of a fixed floor and the
// operator's own active level. Needed since brightness became a slider
// that can go below the old fixed floor (15%) — without this, an active
// level below 15% would make the screen get BRIGHTER when going idle.
constexpr uint8_t IDLE_DIM_FLOOR = 15;
uint8_t idleDimTargetPercent() {
    return activeBrightnessPercent < IDLE_DIM_FLOOR ? activeBrightnessPercent : IDLE_DIM_FLOOR;
}

// Tracks any recognized KeyAction, same basis AUTO_ADVANCE_MS's carousel
// timer uses for "idle". On a keyboardless unit this never advances past
// boot, so the display dims at the configured timeout and stays dimmed —
// the right outcome for an unattended multi-hour drive, not a corner case.
uint32_t lastKeyActivity = 0;

// Without a keyboard the pages rotate on their own — stuck on one page
// during a multi-hour field test is worse than cycling.
constexpr uint32_t AUTO_ADVANCE_MS = 8000;
// Idle redraw cadence (staleness guard).
constexpr uint32_t REDRAW_MS = 1000;
// Redraw cadence while the toast or RX pulse is animating — a bounded
// burst (TOAST_DURATION_MS or RX_PULSE_MS), not a continuous loop.
constexpr uint32_t FAST_REDRAW_MS = 60;

// Field Analyzer's Scope view (Phase 10) is the one page whose mere arrival
// requests a radio action (docs/research/LoRaTrace-Phases-7-10-Design.md
// §8.1: "entering it requests the bounded radio-owned SCOPE_ACQUIRE mode" —
// every other Analyzer view stays purely passive). Samples the currently
// active channel's own frequency, the same "explicitly displayed frequency"
// drawChannelPage() already shows via radioActiveChannel(). Gated on
// keyboardReady: a headless unit's own auto-advance carousel (AUTO_ADVANCE_MS
// below) would otherwise pause Watch for ~4.8s every lap it happens to cycle
// through Scope, silently costing coverage on exactly the deployment mode
// (unattended, multi-hour) that most needs continuous Watch — Scope is an
// interactive exploration tool, not something a headless run should trigger
// on its own. A no-op if a capture is already running (radioRequestScopeAcquire()
// would just queue a cancel instead) or nothing else can run right now
// (Trace paused, another bounded action active) — the operator can still
// trigger it explicitly (SCOPE_TOGGLE, below) once whatever's in the way
// clears.
void maybeStartScopeAcquire() {
    if (activeView() != UiPage::SCOPE || !keyboardReady || radioScopeAcquireIsActive()) return;
    const uint32_t freqKhz = (uint32_t)(radioActiveChannel().freq_mhz * 1000.0f + 0.5f);
    radioRequestScopeAcquire(freqKhz);
}

// The operator-facing main carousel, explicit rather than an enum-value
// range (originally so a hub page could be added without renumbering
// anything). PROBE/SWEEP/CELL/METER..NODES are real UiPage values but not
// carousel stops: each is a view of the one card that owns it (CARD_VIEWS
// below), reached with up/down, never through prev/next paging and — since
// 2026-09-06 — no longer through a menu row either.
constexpr UiPage MAIN_PAGES[] = {
    UiPage::RADIO, UiPage::ACTIVITY, UiPage::CHANNEL, UiPage::GPS, UiPage::SYSTEM,
};
constexpr uint8_t MAIN_PAGE_COUNT = (uint8_t)(sizeof(MAIN_PAGES) / sizeof(MAIN_PAGES[0]));

// Card views (2026-09-05). Each main-carousel card owns an ordered list of
// pages rather than one; up/down cycles them, left/right still moves the
// carousel. Generalizes what Activity shipped the same day as three hardcoded
// views: a card owns a question, and its views answer that question at
// different resolutions, with the bounded action that refreshes the answer on
// Enter (cardSelectAction() below).
//
//   Radio    the receive chain, three time scales: counters, live level, burst
//   Activity what is out there: summary, live scan, history, one-bin dwell
//   Channel  what I am tuned to, what it yielded, and what else I could be on
//   GPS      where I am, and what kind of place it is (cell-band occupancy)
//   System   the device
//
// The view tokens are UiPage values, not a parallel enum, so each view reuses
// the existing draw function and header name verbatim and the former
// pages that used to stand alone under the Tools/Analyze menu groups keep
// working unchanged; only how you get to them did.
constexpr UiPage RADIO_VIEWS[] = {UiPage::RADIO, UiPage::METER, UiPage::SCOPE};
constexpr UiPage ACTIVITY_VIEWS[] = {UiPage::ACTIVITY, UiPage::SWEEP, UiPage::WATERFALL,
                                     UiPage::FOCUS};
// Probe sits directly after the card (operator request, 2026-09-06): it
// answers "should I be on a different channel", which is the card's own
// question, where Captures and Nodes report what the current one yielded.
constexpr UiPage CHANNEL_VIEWS[] = {UiPage::CHANNEL, UiPage::PROBE, UiPage::CAPTURES,
                                    UiPage::NODES};
constexpr UiPage GPS_VIEWS[] = {UiPage::GPS, UiPage::CELL};
constexpr UiPage SYSTEM_VIEWS[] = {UiPage::SYSTEM};

struct CardViews {
    const UiPage *views;
    uint8_t count;
};

// Same shape as menuItemCount() above, generic over element type so it counts
// both a card's view list and the CARD_VIEWS table itself.
template <typename T, size_t N>
constexpr uint8_t viewCount(const T (&)[N]) {
    return (uint8_t)N;
}

// Index-aligned with MAIN_PAGES — CARD_VIEWS[i] belongs to MAIN_PAGES[i], and
// its first entry must be that card's own page (view 0 is always the card
// itself). Both invariants are asserted below rather than trusted: this table
// is edited by hand every time a tool moves between cards.
constexpr CardViews CARD_VIEWS[] = {
    {RADIO_VIEWS, viewCount(RADIO_VIEWS)},     {ACTIVITY_VIEWS, viewCount(ACTIVITY_VIEWS)},
    {CHANNEL_VIEWS, viewCount(CHANNEL_VIEWS)}, {GPS_VIEWS, viewCount(GPS_VIEWS)},
    {SYSTEM_VIEWS, viewCount(SYSTEM_VIEWS)},
};
static_assert(viewCount(CARD_VIEWS) == MAIN_PAGE_COUNT,
              "CARD_VIEWS and MAIN_PAGES disagree on how many cards there are");
static_assert(RADIO_VIEWS[0] == MAIN_PAGES[0] && ACTIVITY_VIEWS[0] == MAIN_PAGES[1] &&
                  CHANNEL_VIEWS[0] == MAIN_PAGES[2] && GPS_VIEWS[0] == MAIN_PAGES[3] &&
                  SYSTEM_VIEWS[0] == MAIN_PAGES[4],
              "every card's view 0 must be the card's own page");

// Which view each card was last left on, so returning to a card returns to
// where you were rather than resetting to view 0 — the difference between
// glancing away from a running sweep and losing your place in it.
uint8_t cardViewIdx[MAIN_PAGE_COUNT] = {};

// Captures inspector modal state. Index is a recency index into the ring
// (0 = newest), clamped on use rather than on set, because the ring can grow
// underneath an open modal.
bool captureInspectOpen = false;
uint8_t captureInspectIdx = 0;

// The modal is scoped to whichever view is showing the Captures list, so any
// navigation away from that view dismisses it — otherwise it would reappear
// over an unrelated card the next time that view came back around.
void closeCaptureInspect() { captureInspectOpen = false; }



uint8_t mainPageIndex(UiPage p) {
    for (uint8_t i = 0; i < MAIN_PAGE_COUNT; i++) {
        if (MAIN_PAGES[i] == p) return i;
    }
    return 0; // p wasn't a main page — shouldn't happen, fail to Radio's slot
}

bool isMainPage(UiPage p) {
    for (uint8_t i = 0; i < MAIN_PAGE_COUNT; i++) {
        if (MAIN_PAGES[i] == p) return true;
    }
    return false;
}

// Position of `view` among `card`'s views, or -1 if that card can't show it.
// int8_t rather than a bool + separate lookup because both callers want the
// index: one to switch to it, one only to know it exists.
int8_t cardViewSlot(UiPage card, UiPage view) {
    if (!isMainPage(card)) return -1;
    const CardViews &cv = CARD_VIEWS[mainPageIndex(card)];
    for (uint8_t i = 0; i < cv.count; i++) {
        if (cv.views[i] == view) return (int8_t)i;
    }
    return -1;
}

void stepCardView(int8_t delta) {
    const uint8_t card = mainPageIndex(page);
    const uint8_t count = CARD_VIEWS[card].count;
    cardViewIdx[card] = (uint8_t)((cardViewIdx[card] + count + delta) % count);
    closeCaptureInspect();
    maybeStartScopeAcquire();
}

// No fillScreen() here: drawPage() (ui_pages.cpp) already wipes and
// redraws the whole content region every call, and the caller always
// follows a page change with fullRedraw() in the same loop iteration. An
// explicit clear here was a redundant second full-panel blank — the direct
// cause of a visible black flash on every page change (2026-08-25 bench).
// Only ever called with a page already in MAIN_PAGES (RADIO/CHANNEL/GPS/
// SYSTEM) — Probe/Sweep/Cell/Meter/Waterfall/Scope/Captures/Nodes are
// menu-reached islands now (2026-09-05), not part of this loop; their own
// key-handling branch below reopens the menu instead of calling this.
void nextPage() {
    const uint8_t idx = mainPageIndex(page);
    page = MAIN_PAGES[(idx + 1) % MAIN_PAGE_COUNT];
    lastPageChange = millis();
    closeCaptureInspect();
    maybeStartScopeAcquire();
}

void prevPage() {
    const uint8_t idx = mainPageIndex(page);
    page = MAIN_PAGES[(idx + MAIN_PAGE_COUNT - 1) % MAIN_PAGE_COUNT];
    lastPageChange = millis();
    closeCaptureInspect();
    maybeStartScopeAcquire();
}

void jumpToPage(UiPage p) {
    page = p;
    lastPageChange = millis();
    closeCaptureInspect();
    maybeStartScopeAcquire();
}

// The one rule for Enter on a card: run the bounded action that refreshes
// what this view is showing. Keyed on the resolved view, not the card, so
// Radio's Scope view re-acquires a trace while Radio's own counters view
// pauses/resumes Watch — in both cases the key changes the number in front of
// you. These are the plain toggles the island pages already fire; staying put
// rather than navigating to the island page is showResultsPage()'s job now,
// not a separate per-card MenuAction (which is what ACTIVITY_SWEEP_TOGGLE was
// before this generalized).
//
// CAPTURES is absent deliberately: Enter there opens the inspector modal, not
// a radio action, and is handled before this table is consulted. NODES has no
// action of its own — the roster is filled by Watch, which Radio already owns.
MenuAction cardSelectAction(UiPage view) {
    switch (view) {
        case UiPage::RADIO:
        case UiPage::METER: return MenuAction::TRACE_TOGGLE;
        case UiPage::SCOPE: return MenuAction::SCOPE_TOGGLE;
        case UiPage::ACTIVITY:
        case UiPage::SWEEP:
        case UiPage::WATERFALL: return MenuAction::SWEEP_TOGGLE;
        case UiPage::FOCUS: return MenuAction::FOCUS_TOGGLE;
        case UiPage::CHANNEL:
        case UiPage::PROBE: return MenuAction::PROBE_TOGGLE;
        case UiPage::GPS:
        case UiPage::CELL: return MenuAction::CELL_TOGGLE;
        case UiPage::SYSTEM: return MenuAction::SD_RETRY;
        default: return MenuAction::NONE;
    }
}

// R, same rule one level up: keep doing it. Only the two band sweeps have a
// repeat mode — Probe deliberately has none (operator decision, "Repeat only
// on the Sweeps", see ui_menu.h's CELL_REPEAT_TOGGLE comment).
MenuAction cardRepeatAction(UiPage view) {
    switch (view) {
        case UiPage::ACTIVITY:
        case UiPage::SWEEP:
        case UiPage::WATERFALL: return MenuAction::SWEEP_REPEAT_TOGGLE;
        case UiPage::GPS:
        case UiPage::CELL: return MenuAction::CELL_REPEAT_TOGGLE;
        default: return MenuAction::NONE;
    }
}

// The verb an empty view names when telling the operator which key fills it,
// and the global hotkey that fires the same action from anywhere. Both are
// lookups on MenuAction rather than strings typed into each page, because
// that is exactly how the old per-page hints went stale: "Enter: start repeat
// Sweep" survived on the Waterfall page across the split that made Enter
// single-shot and R repeat. A hint built from the same tables the key handler
// dispatches on cannot drift from what the key actually does.
const char *cardActionVerb(MenuAction action) {
    switch (action) {
        case MenuAction::SWEEP_TOGGLE: return "sweep";
        case MenuAction::SWEEP_REPEAT_TOGGLE: return "repeat";
        case MenuAction::PROBE_TOGGLE: return "probe";
        case MenuAction::CELL_TOGGLE: return "scan";
        case MenuAction::CELL_REPEAT_TOGGLE: return "repeat";
        case MenuAction::FOCUS_TOGGLE: return "survey";
        case MenuAction::SCOPE_TOGGLE: return "capture";
        // TRACE_TOGGLE and SD_RETRY are deliberately absent: neither produces
        // the data a view is missing, so neither belongs in an empty state's
        // hint. Those views explain what fills them instead (ui_pages.cpp).
        default: return nullptr;
    }
}

char cardActionGlobalKey(MenuAction action) {
    switch (action) {
        case MenuAction::PROBE_TOGGLE: return 'P';
        case MenuAction::SWEEP_TOGGLE: return 'S';
        case MenuAction::CELL_TOGGLE: return 'C';
        default: return '\0';
    }
}

// Drains the TCA8418 event FIFO and returns the most recently recognized
// KeyAction this poll (keyboard.h), or NONE. Several actions queued between
// polls collapse to the last one — acceptable at a 30ms poll interval for
// sparse, deliberate keypresses.
KeyAction pollKeyAction() {
    if (!keyboardReady) return KeyAction::NONE;
    KeyAction result = KeyAction::NONE;
    while (keys.available() > 0) {
        const uint8_t raw = (uint8_t)keys.getEvent();
        if (keyDumpEnabled) keyDumpEmit(raw);
        const KeyAction a = keyboardDecodeEvent(raw);
        if (a != KeyAction::NONE) result = a;
    }
    return result;
}

// NOTE: no startWrite()/endWrite() batching here despite looking like the
// obvious next step — Arduino_GFX's fillRect()/print() etc. already each
// wrap themselves in their own startWrite()/endWrite(), and a second outer
// startWrite() around a sequence of such calls deadlocks on the first
// nested call (verified against the vendored GFX/SPI sources, caught
// before it became a hang on first boot). Moot anyway: uiTft->flush()
// below is the real single-transaction boundary over the whole composed
// frame, and it isn't nested inside anything.
// Worst and cumulative frame cost since boot. The open question this exists to
// answer is whether the card rebuild's extra per-view draw paths cost anything
// measurable while a bounded action owns the radio (docs/STATUS.md) — which is
// unanswerable by eye, since the expensive frames are exactly the ones an
// operator is least likely to be watching. Two uint32s and a counter; the draw
// itself is unchanged.
//
// Deliberately not split per page: the question is whether ANY frame got slow,
// and a per-page table would cost more RAM than the answer is worth. Serial
// Control's STATUS carries it, so a fixture can sample it without a display.
volatile uint32_t uiRedrawMaxUs = 0;
// uint64: at the measured ~2.6 fps and ~62 ms mean, a uint32 of microseconds
// wraps in about 7.4 hours — run0089 caught it doing exactly that just before
// the 8-hour mark, where the reported mean fell from 62,781 to 5,306 us. The
// max is a running maximum and was unaffected; only the mean was wrong, and
// only after the wrap.
volatile uint64_t uiRedrawTotalUs = 0;
volatile uint32_t uiRedrawCount = 0;

void fullRedraw() {
    const uint32_t started = micros();
    drawHeader();
    drawPage();
    uiTft->flush();
    // micros() wraps every ~71 minutes; an unsigned difference stays correct
    // across the wrap, which a signed comparison would not.
    const uint32_t elapsed = micros() - started;
    if (elapsed > uiRedrawMaxUs) uiRedrawMaxUs = elapsed;
    uiRedrawTotalUs += elapsed;
    uiRedrawCount++;
}

void uiTask(void *) {
    memoryStatsRegisterCurrentTask(MemoryTask::UI);
    fullRedraw();

    uint32_t lastRedraw = millis();
    lastPageChange = lastRedraw;
    lastKeyActivity = lastRedraw;
    uint32_t lastRxSeen = radioPacketCount();
    uint32_t lastProbeRunSeen = radioDiscoverySweepCount();
    uint32_t lastProbeCancelSeen = radioDiscoveryCancelCount();
    uint32_t lastProbeFailureSeen = radioDiscoveryFailureCount();
    uint32_t lastEnergyRunSeen = radioEnergySweepCount();
    uint32_t lastEnergyCancelSeen = radioEnergyCancelCount();
    uint32_t lastEnergyFailureSeen = radioEnergyFailureCount();
    uint32_t lastCellRunSeen = radioCellSweepCount();
    uint32_t lastCellCancelSeen = radioCellCancelCount();
    uint32_t lastCellFailureSeen = radioCellFailureCount();
    uint32_t lastScopeRunSeen = radioScopeAcquireCount();
    uint32_t lastScopeCancelSeen = radioScopeCancelCount();
    uint32_t lastScopeFailureSeen = radioScopeFailureCount();
    bool wasAnimating = false;

    for (;;) {
        serialControlPoll();
        const KeyAction action = pollKeyAction();
        bool redraw = false;

        // Idle-dim: any key resets the idle clock and undims immediately
        // if the display was dimmed. Checked before carousel/menu dispatch
        // so a keypress that also does something else still counts as
        // activity. idleTimeoutIndex == 0 ("Off") disables idle-dim.
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

        // Detect new RX activity every loop, independent of any keypress —
        // this drives the header pulse dot and RADIO's flash bar.
        const uint32_t rxNow = radioPacketCount();
        if (rxNow != lastRxSeen) {
            lastRxSeen = rxNow;
            rxPulseUntil = millis() + RX_PULSE_MS;
        }

        // A small fixed CAD plan can complete before the normal one-second
        // page redraw. Surface the radio-owned completion so it cannot look
        // like the Probe action did nothing.
        const uint32_t probeRuns = radioDiscoverySweepCount();
        if (probeRuns != lastProbeRunSeen) {
            lastProbeRunSeen = probeRuns;
            const bool failed = radioDiscoveryFailureCount() != lastProbeFailureSeen;
            const bool cancelled = radioDiscoveryCancelCount() != lastProbeCancelSeen;
            lastProbeFailureSeen = radioDiscoveryFailureCount();
            lastProbeCancelSeen = radioDiscoveryCancelCount();
            char msg[48];
            if (failed) {
                snprintf(msg, sizeof(msg), "Probe: FAILED %d", radioLastError());
            } else if (cancelled) {
                snprintf(msg, sizeof(msg), "Probe: CANCELLED");
            } else {
                snprintf(msg, sizeof(msg), "Probe: DONE %u in %lums",
                         (unsigned)radioDiscoveryCandidateCount(),
                         (unsigned long)radioDiscoveryLastAwayMs());
            }
            showToast(msg);
            probeTerminalShownAt = millis();
            redraw = true;
        }

        // Same async-completion-toast shape as Probe's block above, for
        // Sweep — but suppressed while repeat mode is actively chaining
        // laps (operator request, 2026-08-29): a toast per lap would be
        // constant noise for a "walk around and scan" session, and the
        // Sweep page's own on-screen lap counter already covers it. The
        // final lap (whatever stopped the chain — operator Ctrl+S, a
        // failure, Trace pausing) still toasts normally, since
        // radioEnergySweepRepeatIsActive() has already gone false by the
        // time the radio task hands control back here.
        const uint32_t energyRuns = radioEnergySweepCount();
        if (energyRuns != lastEnergyRunSeen) {
            lastEnergyRunSeen = energyRuns;
            const bool energyFailed = radioEnergyFailureCount() != lastEnergyFailureSeen;
            const bool energyCancelled = radioEnergyCancelCount() != lastEnergyCancelSeen;
            lastEnergyFailureSeen = radioEnergyFailureCount();
            lastEnergyCancelSeen = radioEnergyCancelCount();
            if (!radioEnergySweepRepeatIsActive()) {
                char energyMsg[48];
                if (energyFailed) {
                    snprintf(energyMsg, sizeof(energyMsg), "Sweep: FAILED %d", radioLastError());
                } else if (energyCancelled) {
                    snprintf(energyMsg, sizeof(energyMsg), "Sweep: CANCELLED");
                } else {
                    snprintf(energyMsg, sizeof(energyMsg), "Sweep: DONE %u peaks in %lums",
                             (unsigned)radioEnergyPeakCount(), (unsigned long)radioEnergyLastAwayMs());
                }
                showToast(energyMsg);
            }
            sweepTerminalShownAt = millis();
            redraw = true;
        }

        // Same async-completion-toast shape as Probe/Sweep above, now that
        // Cell has its own card (Phase 11): sets cellTerminalShownAt for
        // drawCellPage()'s IDLE-after-hold reversion, same as
        // probeTerminalShownAt/sweepTerminalShownAt above.
        const uint32_t cellRuns = radioCellSweepCount();
        if (cellRuns != lastCellRunSeen) {
            lastCellRunSeen = cellRuns;
            const bool cellFailed = radioCellFailureCount() != lastCellFailureSeen;
            const bool cellCancelled = radioCellCancelCount() != lastCellCancelSeen;
            lastCellFailureSeen = radioCellFailureCount();
            lastCellCancelSeen = radioCellCancelCount();
            char cellMsg[48];
            if (cellFailed) {
                snprintf(cellMsg, sizeof(cellMsg), "Cell: FAILED %d", radioLastError());
            } else if (cellCancelled) {
                snprintf(cellMsg, sizeof(cellMsg), "Cell: CANCELLED");
            } else {
                const CellStrongestSignal strongest = radioCellStrongestSignal();
                if (strongest.valid) {
                    snprintf(cellMsg, sizeof(cellMsg), "Cell: DONE %.1fMHz %.1fdBm",
                             (double)strongest.freq_mhz, (double)strongest.rssi_peak_dbm_x10 / 10.0);
                } else {
                    snprintf(cellMsg, sizeof(cellMsg), "Cell: DONE");
                }
            }
            showToast(cellMsg);
            cellTerminalShownAt = millis();
            redraw = true;
        }

        // Same async-completion-toast shape as Probe/Sweep/Cell above, for
        // Field Analyzer's Scope view (Phase 10). No repeat-mode suppression
        // to mirror (Scope has none) and no SD-related state to report — a
        // capture never writes to SD, it only fills the in-RAM ScopeTrace.
        const uint32_t scopeRuns = radioScopeAcquireCount();
        if (scopeRuns != lastScopeRunSeen) {
            lastScopeRunSeen = scopeRuns;
            const bool scopeFailed = radioScopeFailureCount() != lastScopeFailureSeen;
            const bool scopeCancelled = radioScopeCancelCount() != lastScopeCancelSeen;
            lastScopeFailureSeen = radioScopeFailureCount();
            lastScopeCancelSeen = radioScopeCancelCount();
            char scopeMsg[48];
            if (scopeFailed) {
                snprintf(scopeMsg, sizeof(scopeMsg), "Scope: FAILED %d", radioLastError());
            } else if (scopeCancelled) {
                snprintf(scopeMsg, sizeof(scopeMsg), "Scope: CANCELLED");
            } else {
                snprintf(scopeMsg, sizeof(scopeMsg), "Scope: DONE in %lums",
                         (unsigned long)radioScopeLastAwayMs());
            }
            showToast(scopeMsg);
            scopeTerminalShownAt = millis();
            redraw = true;
        }

        // P is deliberately global rather than card- or menu-scoped: it is
        // the one hard shortcut for the bounded Probe start/cancel action.
        // showProbeResults() closes any open menu after an accepted request.
        if (action == KeyAction::PROBE) {
            fireMenuAction(MenuAction::PROBE_TOGGLE);
            redraw = true;
        } else if (action == KeyAction::SWEEP) {
            // Same global-shortcut shape as P/Probe — works from any UI
            // state. Whether it navigates to the Sweep card or stays put is
            // showResultsPage()'s call now, not a second MenuAction's: firing
            // S from Radio should show you what you just started, firing it
            // from Activity should not move you off the page you fired it to
            // watch. That used to be this branch's own ACTIVITY_SWEEP_TOGGLE
            // special case (2026-09-05, removed once every card could carry a
            // Sweep view).
            fireMenuAction(MenuAction::SWEEP_TOGGLE);
            redraw = true;
        } else if (action == KeyAction::CELL) {
            // Same global-shortcut shape as P/Probe and S/Sweep above
            // (Phase 11, 2026-09-01).
            fireMenuAction(MenuAction::CELL_TOGGLE);
            redraw = true;
        } else if (!menu.isOpen()) {
            // Carousel: page navigation is this file's own concern, not
            // MenuState's (ui_menu.h stays free of any UiPage dependency).
            // JUMP_1..5 are hoisted ahead of the page-mode branches below —
            // a direct jump to a named page works identically regardless of
            // which card view is currently showing — a digit always means
            // the card, never its view. Five now (2026-09-05,
            // ACTIVITY joined at slot 2), JUMP_6 unmapped — MAIN_PAGES
            // above is the actual source of truth for what "the main
            // carousel" means; this switch just names each slot.
            if (action == KeyAction::JUMP_1) {
                jumpToPage(UiPage::RADIO);
                redraw = true;
            } else if (action == KeyAction::JUMP_2) {
                jumpToPage(UiPage::ACTIVITY);
                redraw = true;
            } else if (action == KeyAction::JUMP_3) {
                jumpToPage(UiPage::CHANNEL);
                redraw = true;
            } else if (action == KeyAction::JUMP_4) {
                jumpToPage(UiPage::GPS);
                redraw = true;
            } else if (action == KeyAction::JUMP_5) {
                jumpToPage(UiPage::SYSTEM);
                redraw = true;
            } else if (activeView() == UiPage::CAPTURES && captureInspectOpen) {
                // Hoisted above the card branch below: the modal owns the keys
                // while it is open, so UP/DOWN browse the ring rather than
                // changing card view, and BACK closes the modal rather than
                // opening the menu.
                CaptureHistory history;
                const uint8_t count = analyzerCaptureHistorySnapshot(history, pdMS_TO_TICKS(20))
                                          ? history.count : 0;
                if (action == KeyAction::BACK || action == KeyAction::SELECT) {
                    captureInspectOpen = false;
                    redraw = true;
                } else if (action == KeyAction::UP || action == KeyAction::PREV) {
                    if (count > 0 && captureInspectIdx + 1 < count) captureInspectIdx++;
                    redraw = true;
                } else if (action == KeyAction::DOWN || action == KeyAction::NEXT) {
                    if (captureInspectIdx > 0) captureInspectIdx--;
                    redraw = true;
                }
            } else {
                // A main-carousel card. Left/right always moves the carousel;
                // up/down cycles this card's own views (stepCardView()), the
                // same way the carousel cycles the cards themselves.
                //
                // The exception is a card with a single view (System): there
                // is nothing to cycle, so up/down keeps aliasing prev/next
                // page exactly as it did before card views existed —
                // preserving the printed Fn-arrow diamond's "doubles as page
                // nav" behavior (Phase 5) wherever it still has a job to do,
                // rather than making two of its four keys dead on that card.
                const uint8_t views = CARD_VIEWS[mainPageIndex(page)].count;
                const UiPage view = activeView();
                if (action == KeyAction::UP && views > 1) {
                    stepCardView(-1);
                    redraw = true;
                } else if (action == KeyAction::DOWN && views > 1) {
                    stepCardView(1);
                    redraw = true;
                } else if (action == KeyAction::PREV || action == KeyAction::UP) {
                    prevPage();
                    redraw = true;
                } else if (action == KeyAction::NEXT || action == KeyAction::DOWN) {
                    nextPage();
                    redraw = true;
                } else if (action == KeyAction::BACK) {
                    menu.open();
                    redraw = true;
                } else if (action == KeyAction::SELECT && view == UiPage::CAPTURES) {
                    // The list itself has no cursor — up/down is spoken for by
                    // the card's views — so the modal is the browser.
                    captureInspectIdx = 0; // newest first
                    captureInspectOpen = true;
                    redraw = true;
                } else if (action == KeyAction::SELECT) {
                    const MenuAction fire = cardSelectAction(view);
                    if (fire != MenuAction::NONE) fireMenuAction(fire);
                    redraw = true;
                } else if (action == KeyAction::REPEAT) {
                    const MenuAction fire = cardRepeatAction(view);
                    if (fire != MenuAction::NONE) fireMenuAction(fire);
                    redraw = true;
                }
            }
        } else if (action != KeyAction::NONE) {
            // Menu open (root/group/slider) — MenuState owns navigation;
            // this file only reacts to what fired. Captured before handle()
            // runs: leaving a slider (BACK or SELECT, SLIDER -> ROOT —
            // ui_menu.h's handleSlider() treats both the same way,
            // 2026-08-29) is the debounce point for persisting it (see
            // BRIGHTNESS_UP/DOWN in ui_actions.cpp for why saves don't
            // happen every step). Which slider is captured too (via its
            // sliderIncrease action, unique per slider) — inSlider() flips
            // false the instant handle() below processes this same
            // BACK/SELECT, so menu.currentItem() must be read before that
            // call, not after.
            const bool leavingSlider = menu.inSlider() &&
                                       (action == KeyAction::BACK || action == KeyAction::SELECT);
            const MenuAction leavingSliderKind = leavingSlider ? menu.currentItem().sliderIncrease : MenuAction::NONE;
            // UP/DOWN (';'/'.', split off PREV/NEXT 2026-09-03 for the
            // Analyze hub — see keyboard.h's KeyAction::UP comment)
            // translate back to PREV/NEXT here so the settings menu and
            // slider keep responding to all four Fn-arrow diamond keys
            // exactly as before the split — ui_menu.h stays a plain two-
            // action PREV/NEXT model, unaware any of this happened.
            const KeyAction menuAction = action == KeyAction::UP     ? KeyAction::PREV
                                        : action == KeyAction::DOWN  ? KeyAction::NEXT
                                                                      : action;
            const MenuAction fired = menu.handle(menuAction);
            if (fired != MenuAction::NONE) fireMenuAction(fired);
            if (leavingSliderKind == MenuAction::SWEEP_MARGIN_UP) {
                SweepMarginSettings settings;
                settings.margin_dbm_x10 = radioEnergySweepMarginDbmX10();
                writeSweepMarginSettingsToSD(settings);
            } else if (leavingSlider) {
                DisplaySettings settings;
                settings.brightness_pct = activeBrightnessPercent;
                settings.idle_timeout_index = idleTimeoutIndex;
                writeDisplaySettingsToSD(settings);
            }
            redraw = true;
        }

        const bool animating = (toastMsg[0] != '\0' && toastActive()) || rxPulseActive();
        const uint32_t redrawInterval = animating ? FAST_REDRAW_MS : REDRAW_MS;

        // Every tick goes through fullRedraw(), including the fast-redraw
        // burst — redrawing into the off-screen canvas costs nothing the
        // viewer can see, since uiTft->flush() is the only point anything
        // reaches the glass, as one atomic blit.
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
            // Toast/RX pulse just expired — force one more redraw so the
            // overlay clears immediately rather than lingering until the
            // next periodic tick (up to ~1s stale).
            fullRedraw();
            lastRedraw = millis();
        }
        wasAnimating = animating;

        if (toastMsg[0] != '\0' && !toastActive()) {
            toastMsg[0] = '\0';
        }

        // Poll rather than use the INT pin on GPIO11 — I2C isn't
        // interrupt-safe, and 30ms polling feels immediate anyway.
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

} // namespace

// 1-based position of whichever main-carousel stop `page` currently is.
// Deliberately not raw UiPage ordinals/UiPage::COUNT: those would count
// every card view as if it were its own top-level stop, showing e.g. "8/14"
// for something prev/next/digit keys can't reach (operator report,
// 2026-09-04: "users will think they are missing cards").
//
// The 0 return is now unreachable — `page` is always a card since the
// Tools/Analyze menu rows stopped opening pages standalone (2026-09-06) —
// but both it and drawFooterStatus()'s guard on it stay: it costs one
// compare, and it is the difference between a wrong "1/5" and no position
// at all if a future page ever sits outside the carousel again.
uint8_t mainCarouselPosition() {
    for (uint8_t i = 0; i < MAIN_PAGE_COUNT; i++) {
        if (MAIN_PAGES[i] == page) return (uint8_t)(i + 1);
    }
    return 0;
}

uint8_t mainCarouselCount() {
    return MAIN_PAGE_COUNT;
}

uint32_t uiRedrawWorstUs() { return uiRedrawMaxUs; }
uint32_t uiRedrawMeanUs() {
    const uint32_t n = uiRedrawCount;
    return n ? (uint32_t)(uiRedrawTotalUs / n) : 0;
}
// Frame count is exposed so a host can tell a genuine mean from one computed
// over too few frames, and can difference two samples for a windowed mean
// rather than reading the since-boot average.
uint32_t uiRedrawFrames() { return uiRedrawCount; }
void uiRedrawStatsReset() {
    uiRedrawMaxUs = 0;
    uiRedrawTotalUs = 0;
    uiRedrawCount = 0;
}

// The page a card is currently rendering: the card's own on view 0, one of
// the pages it owns (CARD_VIEWS) otherwise. The non-card fallback is the
// same unreachable-but-kept case as mainCarouselPosition()'s 0 above.
UiPage activeView() {
    if (!isMainPage(page)) return page;
    const uint8_t card = mainPageIndex(page);
    return CARD_VIEWS[card].views[cardViewIdx[card]];
}

uint8_t activeViewIndex() { return isMainPage(page) ? cardViewIdx[mainPageIndex(page)] : 0; }

// "Enter/S: sweep   R: repeat" for a view, or nullptr where neither key does
// anything that would fill it. Generated, never typed — see cardActionVerb().
const char *cardHintLine(UiPage view) {
    static char buf[40];
    const MenuAction sel = cardSelectAction(view);
    const MenuAction rep = cardRepeatAction(view);
    const char *selVerb = cardActionVerb(sel);
    const char *repVerb = cardActionVerb(rep);
    if (selVerb == nullptr && repVerb == nullptr) return nullptr;

    int n = 0;
    if (selVerb != nullptr) {
        const char key = cardActionGlobalKey(sel);
        if (key != '\0') {
            n = snprintf(buf, sizeof(buf), "Enter/%c: %s", key, selVerb);
        } else {
            n = snprintf(buf, sizeof(buf), "Enter: %s", selVerb);
        }
    }
    if (repVerb != nullptr && n >= 0 && (size_t)n < sizeof(buf)) {
        snprintf(buf + n, sizeof(buf) - (size_t)n, "%sR: %s", n > 0 ? "   " : "", repVerb);
    }
    return buf;
}

uint8_t activeViewCount() { return isMainPage(page) ? CARD_VIEWS[mainPageIndex(page)].count : 0; }

// Where a bounded action's "show me the result" lands. Every result page is
// some card's view now (CARD_VIEWS), so this navigates to the card that owns
// it and selects that view — firing S from Radio lands on Activity's Sweep
// view, P from GPS on Channel's Probe view. Nothing reaches a page that has
// no carousel position any more, which is what let the Tools/Analyze menu
// groups and their island-page navigation go (2026-09-06).
//
// Staying put when the current card already carries the view is the one
// exception, and the important one: on Activity the dashboard *is* the view
// built to watch a sweep run, so starting one must not yank off it. That is
// what Activity's own ACTIVITY_SWEEP_TOGGLE used to buy for one page.
void showResultsPage(UiPage p) {
    menu.close();
    if (cardViewSlot(page, p) >= 0) return;
    for (uint8_t i = 0; i < MAIN_PAGE_COUNT; i++) {
        const int8_t slot = cardViewSlot(MAIN_PAGES[i], p);
        if (slot < 0) continue;
        cardViewIdx[i] = (uint8_t)slot;
        jumpToPage(MAIN_PAGES[i]);
        return;
    }
    jumpToPage(p); // no card claims it — can't happen while CARD_VIEWS is total
}

void showProbeResults() { showResultsPage(UiPage::PROBE); }

void showSweepResults() { showResultsPage(UiPage::SWEEP); }

void showCellResults() { showResultsPage(UiPage::CELL); }

bool captureInspectIsOpen() { return captureInspectOpen; }
uint8_t captureInspectIndex() { return captureInspectIdx; }

void showFocusResults() { showResultsPage(UiPage::FOCUS); }

// menu's constructor needs ROOT_ITEMS/ROOT_COUNT, fine to reference here
// even though this definition needs external linkage (ui_pages.cpp/
// ui_actions.cpp both use `menu` directly) — linkage is a per-declaration
// property, not a scoping restriction.
MenuState menu(ROOT_ITEMS, ROOT_COUNT);

void showToast(const char *msg) {
    strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
    toastMsg[sizeof(toastMsg) - 1] = '\0';
    toastShownAt = millis();
}

bool toastActive() {
    return toastMsg[0] != '\0' && (millis() - toastShownAt) < TOAST_DURATION_MS;
}

bool rxPulseActive() {
    return millis() < rxPulseUntil;
}

bool uiTaskStart(Arduino_GFX *gfx, const DisplaySettings &settings) {
    if (gfx == nullptr) return false;

    // Seed from main.cpp's boot-time SD load (display_settings.h), not
    // this file's hardcoded defaults — clamped defensively since these
    // values go straight into backlightSetPercent() below. A brand-new/
    // empty SD card leaves `settings` at struct defaults (100%, 60s).
    activeBrightnessPercent = settings.brightness_pct;
    if (activeBrightnessPercent < BRIGHTNESS_MIN) activeBrightnessPercent = BRIGHTNESS_MIN;
    if (activeBrightnessPercent > BRIGHTNESS_MAX) activeBrightnessPercent = BRIGHTNESS_MAX;
    idleTimeoutIndex = settings.idle_timeout_index;
    if (idleTimeoutIndex >= IDLE_TIMEOUT_OPTION_COUNT) idleTimeoutIndex = 2;
    // main.cpp's backlightInit() (boot splash) always starts at 100% —
    // apply the real loaded level now so it's visible from this task's
    // first frame instead of staying at 100% until the operator touches
    // the Brightness slider.
    backlightSetPercent(activeBrightnessPercent);

    // Direct-to-panel drawing causes real, visible flicker/tearing (Phase 6
    // bench pass, 2026-08-25): every draw call is immediately visible on
    // glass. Fixed the way M5GFX/LovyanGFX sprite UIs get their smoothness:
    // everything draws into this off-screen canvas instead, and nothing
    // reaches the glass until uiTft->flush() blits the whole composed
    // frame in one shot. _Indexed rather than full RGB565: this UI only
    // ever uses 6 colours (ui_pages.cpp), so 1 byte/pixel costs ~32KB
    // instead of RGB565's ~63KB. No PSRAM on this board, so this is a real
    // malloc() against the shared heap budget, decided with the operator.
    // Falls back to drawing straight on the panel (flicker and all) if the
    // allocation fails, rather than taking the whole UI down.
    memoryStatsLog("canvas-before");
    canvas = new Arduino_Canvas_Indexed(gfx->width(), gfx->height(), gfx, 0, 0, 0);
    if (canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        uiTft = canvas;
    } else {
        delete canvas;
        canvas = nullptr;
        uiTft = gfx;
    }
    memoryStatsLog(canvas != nullptr ? "canvas-after" : "canvas-fallback");

    // Wire is already up from ioExpanderInit(); begin() again is harmless
    // and keeps this call self-contained if the boot order ever changes.
    Wire.begin(PIN_IOEXP_SDA, PIN_IOEXP_SCL);
    keyboardReady = keys.begin(KEYBOARD_I2C_ADDR, &Wire);
    if (keyboardReady) {
        keys.matrix(KEYBOARD_MATRIX_ROWS, KEYBOARD_MATRIX_COLS);
        keys.flush(); // discard boot-time noise
    }

    // 5120, raised from 4096 on 2026-09-07 with a measurement behind it — the
    // old number had no recorded rationale at all. run0089's 8-hour soak read
    // ui_stack_free at 288 B, a 93% high-water mark reached in the first two
    // minutes: never an overflow, but no margin either, on a task that has
    // already crashed once from a large draw-path local (the ~5.5 KB
    // WaterfallHistory, see drawWaterfallPage()).
    //
    // Moving the snapshot structs to BSS took ~720 B off the worst draw chain
    // (1,312 -> 592 B), which puts the estimated peak near 3,088 B. At 4096
    // that is still 75% used; at 5120 it is 60%, which leaves room for a deep
    // path the soak never exercised without being so generous that a real
    // regression stops showing up in the watermark.
    //
    // Only ~1,312 B of that peak is this project's own frames. The rest is
    // GFX internals, snprintf and FreeRTOS entry, none of which -fstack-usage
    // can see — which is why the number is set from a measured watermark
    // rather than from summing call frames.
    BaseType_t ok = xTaskCreatePinnedToCore(uiTask, "ui", 5120, nullptr, 1, nullptr, 0);
    return ok == pdPASS;
}

bool uiKeyboardReady() {
    return keyboardReady;
}

void uiKeyDumpSetEnabled(bool enabled) {
    keyDumpEnabled = enabled;
}

bool uiKeyDumpIsEnabled() {
    return keyDumpEnabled;
}
