// LoRaTrace RX — ui_task lifecycle, input, and main loop.
//
// Split out of a single ~1265-line ui_task.cpp (2026-08-25 cleanup pass)
// into three files by concern: this file (task lifecycle, keyboard input,
// main loop, and all operator-facing state — page, menu, toast, RX pulse,
// brightness/idle-dim), drawing (ui_pages.cpp), and menu-action business
// logic (ui_actions.cpp). See ui_task.h for the subsystem design and
// ui_task_shared.h for the contract between these three files.

#include "ui_task.h"
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
// sits right next to Tools > Sweep, the actual radio action, and reusing
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
    {"Connectivity", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, CONNECTIVITY_GROUP_ITEMS, 2},
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
// Tools and Analyze moved from main-carousel hub pages into real menu
// GROUPs (operator report, 2026-09-05: two separate navigation systems on
// screen — a home carousel with card-like hubs, and a BACK-triggered menu
// with a different root — read as "am I in the menu or not". See
// CHANGELOG.md for the full reasoning; this reverses the 2026-09-04 removal
// noted immediately below only for Tools/Analyze, not Probe/Sweep/Cell,
// whose "no duplicate entry point" convention is unchanged — those three
// still have no *root* row, only these two group rows one level up. Trace
// moved into Tools as its first child row (no longer its own root row) —
// it has no global hotkey of its own the way P/S/C do, so it gains nothing
// from staying at the root that a one-level-deeper menu row doesn't already
// give it.
constexpr MenuItem ANALYZE_GROUP_ITEMS[] = {
    {"Meter", ItemKind::ACTION, MenuAction::OPEN_METER, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Waterfall", ItemKind::ACTION, MenuAction::OPEN_WATERFALL, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Scope", ItemKind::ACTION, MenuAction::OPEN_SCOPE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Captures", ItemKind::ACTION, MenuAction::OPEN_CAPTURES, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Nodes", ItemKind::ACTION, MenuAction::OPEN_NODES, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
constexpr MenuItem TOOLS_GROUP_ITEMS[] = {
    {"Trace", ItemKind::ACTION, MenuAction::TRACE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Probe", ItemKind::ACTION, MenuAction::OPEN_PROBE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Sweep", ItemKind::ACTION, MenuAction::OPEN_SWEEP, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Cell", ItemKind::ACTION, MenuAction::OPEN_CELL, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
constexpr MenuItem ROOT_ITEMS[] = {
    {"Profile", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, PROFILE_GROUP_ITEMS, 3},
    {"Analyze", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, ANALYZE_GROUP_ITEMS, 5},
    {"Tools", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, TOOLS_GROUP_ITEMS, 4},
    {"System", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 4},
};
constexpr uint8_t ROOT_COUNT = 4;
// ROOT_ITEMS' own GROUP rows need the same guard as SYSTEM_GROUP_ITEMS'
// above — and more so: the v0.8.9 bug those cite was *here*, on the row
// pointing at SYSTEM_GROUP_ITEMS, not inside it. Asserting only the
// children would have left the exact historical failure uncovered.
// ROOT_COUNT is hand-written for the same reason and drifts the same way.
static_assert(ROOT_ITEMS[0].itemCount == menuItemCount(PROFILE_GROUP_ITEMS),
              "root > Profile itemCount does not match PROFILE_GROUP_ITEMS");
static_assert(ROOT_ITEMS[1].itemCount == menuItemCount(ANALYZE_GROUP_ITEMS),
              "root > Analyze itemCount does not match ANALYZE_GROUP_ITEMS");
static_assert(ROOT_ITEMS[2].itemCount == menuItemCount(TOOLS_GROUP_ITEMS),
              "root > Tools itemCount does not match TOOLS_GROUP_ITEMS");
static_assert(ROOT_ITEMS[3].itemCount == menuItemCount(SYSTEM_GROUP_ITEMS),
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
    if (page != UiPage::SCOPE || !keyboardReady || radioScopeAcquireIsActive()) return;
    const uint32_t freqKhz = (uint32_t)(radioActiveChannel().freq_mhz * 1000.0f + 0.5f);
    radioRequestScopeAcquire(freqKhz);
}

// The operator-facing main carousel, explicit rather than an enum-value
// range (originally so a hub page could be added without renumbering
// anything). Tools/Analyze are no longer carousel stops (2026-09-05 — they
// moved into the menu tree, see ROOT_ITEMS' own comment); PROBE/SWEEP/
// CELL/METER..NODES are real UiPage values (each still needs its own draw
// function and footer identity) but are reached only through the menu
// now, never through prev/next paging. ACTIVITY inserted at slot 2
// (operator request, same day) as a read-only mirror of whichever bounded
// action is currently running — see its own comment on UiPage (ui_task.h).
constexpr UiPage MAIN_PAGES[] = {
    UiPage::RADIO, UiPage::ACTIVITY, UiPage::CHANNEL, UiPage::GPS, UiPage::SYSTEM,
};
constexpr uint8_t MAIN_PAGE_COUNT = (uint8_t)(sizeof(MAIN_PAGES) / sizeof(MAIN_PAGES[0]));

bool isAnalyzeSubPage(UiPage p) {
    return p == UiPage::METER || p == UiPage::WATERFALL || p == UiPage::SCOPE ||
           p == UiPage::CAPTURES || p == UiPage::NODES;
}

bool isToolsSubPage(UiPage p) {
    return p == UiPage::PROBE || p == UiPage::SWEEP || p == UiPage::CELL;
}

uint8_t mainPageIndex(UiPage p) {
    for (uint8_t i = 0; i < MAIN_PAGE_COUNT; i++) {
        if (MAIN_PAGES[i] == p) return i;
    }
    return 0; // p wasn't a main page — shouldn't happen, fail to Radio's slot
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
    maybeStartScopeAcquire();
}

void prevPage() {
    const uint8_t idx = mainPageIndex(page);
    page = MAIN_PAGES[(idx + MAIN_PAGE_COUNT - 1) % MAIN_PAGE_COUNT];
    lastPageChange = millis();
    maybeStartScopeAcquire();
}

void jumpToPage(UiPage p) {
    page = p;
    lastPageChange = millis();
    maybeStartScopeAcquire();
}

// UP/DOWN while on one of Analyze's/Tools' own pages cycles the other pages
// in that same group (menu order — see ANALYZE_GROUP_ITEMS/TOOLS_GROUP_ITEMS
// above), unchanged by the 2026-09-05 hub removal; only how you arrive here
// (a menu row now, not a hub SELECT) and how you leave (BACK/PREV/NEXT
// reopen the menu, see the isAnalyzeSubPage()/isToolsSubPage() key-handling
// branches below) changed.
constexpr UiPage ANALYZE_PAGES[] = {
    UiPage::METER, UiPage::WATERFALL, UiPage::SCOPE, UiPage::CAPTURES, UiPage::NODES,
};
constexpr uint8_t ANALYZE_PAGE_COUNT = (uint8_t)(sizeof(ANALYZE_PAGES) / sizeof(ANALYZE_PAGES[0]));
constexpr UiPage TOOLS_PAGES[] = {
    UiPage::PROBE, UiPage::SWEEP, UiPage::CELL,
};
constexpr uint8_t TOOLS_PAGE_COUNT = (uint8_t)(sizeof(TOOLS_PAGES) / sizeof(TOOLS_PAGES[0]));

void nextAnalyzeSubPage() {
    uint8_t idx = 0;
    for (uint8_t i = 0; i < ANALYZE_PAGE_COUNT; i++) {
        if (ANALYZE_PAGES[i] == page) { idx = i; break; }
    }
    page = ANALYZE_PAGES[(idx + 1) % ANALYZE_PAGE_COUNT];
    lastPageChange = millis();
    maybeStartScopeAcquire();
}

void prevAnalyzeSubPage() {
    uint8_t idx = 0;
    for (uint8_t i = 0; i < ANALYZE_PAGE_COUNT; i++) {
        if (ANALYZE_PAGES[i] == page) { idx = i; break; }
    }
    page = ANALYZE_PAGES[(idx + ANALYZE_PAGE_COUNT - 1) % ANALYZE_PAGE_COUNT];
    lastPageChange = millis();
    maybeStartScopeAcquire();
}

void nextToolsSubPage() {
    uint8_t idx = 0;
    for (uint8_t i = 0; i < TOOLS_PAGE_COUNT; i++) {
        if (TOOLS_PAGES[i] == page) { idx = i; break; }
    }
    page = TOOLS_PAGES[(idx + 1) % TOOLS_PAGE_COUNT];
    lastPageChange = millis();
    maybeStartScopeAcquire();
}

void prevToolsSubPage() {
    uint8_t idx = 0;
    for (uint8_t i = 0; i < TOOLS_PAGE_COUNT; i++) {
        if (TOOLS_PAGES[i] == page) { idx = i; break; }
    }
    page = TOOLS_PAGES[(idx + TOOLS_PAGE_COUNT - 1) % TOOLS_PAGE_COUNT];
    lastPageChange = millis();
    maybeStartScopeAcquire();
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
void fullRedraw() {
    drawHeader();
    drawPage();
    uiTft->flush();
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
            // state.
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
            // whether the operator is currently viewing a Tools/Analyze
            // sub-page or an ordinary carousel page. Five now (2026-09-05,
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
            } else if (isToolsSubPage(page)) {
                // Reached only via Menu > Tools > Probe/Sweep/Cell now
                // (2026-09-05) — no more hub page to fall back to. SELECT/
                // REPEAT here are the same PROBE_TOGGLE/SWEEP_TOGGLE/
                // CELL_TOGGLE/*_REPEAT_TOGGLE dispatch these three have
                // always fired on this page; P/S/C remain global hotkeys
                // regardless — handled earlier in this function, before
                // carousel dispatch even begins. PREV/NEXT (left/right)
                // alias UP/DOWN here (operator request, 2026-09-05: "helps
                // these tool carousels work like the main carousel") —
                // cycle the other two Tools pages (nextToolsSubPage()/
                // prevToolsSubPage()), never trapped; the same alias
                // direction the plain-carousel branch below already uses
                // (UP aliases PREV there). BACK is the sole "leave to the
                // menu" key now, since there's no carousel slot to page to.
                if (action == KeyAction::UP || action == KeyAction::PREV) {
                    prevToolsSubPage();
                    redraw = true;
                } else if (action == KeyAction::DOWN || action == KeyAction::NEXT) {
                    nextToolsSubPage();
                    redraw = true;
                } else if (action == KeyAction::BACK) {
                    menu.open();
                    redraw = true;
                } else if (action == KeyAction::SELECT && page == UiPage::PROBE) {
                    fireMenuAction(MenuAction::PROBE_TOGGLE);
                    redraw = true;
                } else if (action == KeyAction::SELECT && page == UiPage::SWEEP) {
                    fireMenuAction(MenuAction::SWEEP_TOGGLE);
                    redraw = true;
                } else if (action == KeyAction::SELECT && page == UiPage::CELL) {
                    fireMenuAction(MenuAction::CELL_TOGGLE);
                    redraw = true;
                } else if (action == KeyAction::REPEAT && page == UiPage::SWEEP) {
                    // See the original (pre-gating) comment on this dispatch
                    // for the full Ctrl+S/KEY_RAW_R_PRESS history — unchanged
                    // by the move, just relocated.
                    fireMenuAction(MenuAction::SWEEP_REPEAT_TOGGLE);
                    redraw = true;
                } else if (action == KeyAction::REPEAT && page == UiPage::CELL) {
                    fireMenuAction(MenuAction::CELL_REPEAT_TOGGLE);
                    redraw = true;
                }
            } else if (isAnalyzeSubPage(page)) {
                // Reached only via Menu > Analyze > Meter/Waterfall/Scope/
                // Captures/Nodes now (2026-09-05) — exact structural twin of
                // the Tools sub-page branch above; see its comment for the
                // PREV/NEXT-aliases-UP/DOWN, BACK-leaves-to-menu reasoning.
                if (action == KeyAction::UP || action == KeyAction::PREV) {
                    prevAnalyzeSubPage();
                    redraw = true;
                } else if (action == KeyAction::DOWN || action == KeyAction::NEXT) {
                    nextAnalyzeSubPage();
                    redraw = true;
                } else if (action == KeyAction::BACK) {
                    menu.open();
                    redraw = true;
                } else if (action == KeyAction::SELECT && page == UiPage::SCOPE) {
                    // Same dual re-trigger/cancel shape as before — arriving
                    // here already started a first capture on its own
                    // (maybeStartScopeAcquire()); this is for every capture
                    // after that.
                    fireMenuAction(MenuAction::SCOPE_TOGGLE);
                    redraw = true;
                } else if (action == KeyAction::SELECT && page == UiPage::WATERFALL) {
                    // Operator request, 2026-09-04: Waterfall is Sweep's own
                    // history view, so starting/stopping repeat Sweep straight
                    // from here — without a detour through Tools > Sweep —
                    // saves a real trip. See ui_menu.h's own comment on this
                    // action for why it isn't just SWEEP_REPEAT_TOGGLE reused.
                    fireMenuAction(MenuAction::WATERFALL_SWEEP_REPEAT_TOGGLE);
                    redraw = true;
                }
            } else if (action == KeyAction::PREV || action == KeyAction::UP) {
                // UP aliases PREV on every ordinary page — preserves the
                // printed Fn-arrow diamond's original "doubles as page nav"
                // behavior (Phase 5) outside a Tools/Analyze sub-page, where
                // up/down now has its own real meaning instead. Only RADIO/
                // CHANNEL/GPS/SYSTEM reach this branch now.
                prevPage();
                redraw = true;
            } else if (action == KeyAction::NEXT || action == KeyAction::DOWN) {
                nextPage();
                redraw = true;
            } else if (action == KeyAction::BACK) {
                menu.open();
                redraw = true;
            } else if (action == KeyAction::SELECT && page == UiPage::RADIO) {
                fireMenuAction(MenuAction::TRACE_TOGGLE);
                redraw = true;
            }
            // Enter acts on RADIO (Trace) here; Probe/Sweep/Cell's own
            // SELECT/REPEAT dispatch moved to the isToolsSubPage() branch
            // above. Elsewhere both remain a no-op; ESC (BACK) opens the
            // menu.
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
            // BACK that closes the menu entirely (root depth -> 0) returns
            // to the main carousel rather than leaving `page` on a stale
            // Tools/Analyze sub-page left over from before the menu was
            // opened from there (operator report, 2026-09-05: closing all
            // the way out after browsing System, say, re-showed whichever
            // Probe/Sweep/Cell/Meter/... page had been open beforehand
            // instead of the main carousel) — `page` isn't menu state, so
            // MenuState closing has no way to know it needs to change.
            // Only a BACK that just closed the menu triggers this:
            // selecting a Tools/Analyze row (SELECT, via fireMenuAction()
            // above) also closes the menu, but deliberately onto that exact
            // page, and must not be overridden.
            if (action == KeyAction::BACK && !menu.isOpen() &&
                (isToolsSubPage(page) || isAnalyzeSubPage(page))) {
                jumpToPage(UiPage::RADIO);
            }
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

// 1-based position of whichever main-carousel stop `page` currently is, or
// 0 if `page` isn't one at all (a Tools/Analyze sub-page — reached only
// through the menu since 2026-09-05, with no carousel position of its own)
// — drawFooterStatus() (ui_pages.cpp) omits its "N/M" text on that 0,
// rather than showing a stale or misleading position. Deliberately not raw
// UiPage ordinals/UiPage::COUNT: those would count every sub-page as if it
// were its own top-level stop, showing e.g. "8/14" for a page prev/next/
// digit keys can't actually reach (operator report, 2026-09-04: "users
// will think they are missing cards").
uint8_t mainCarouselPosition() {
    for (uint8_t i = 0; i < MAIN_PAGE_COUNT; i++) {
        if (MAIN_PAGES[i] == page) return (uint8_t)(i + 1);
    }
    return 0;
}

uint8_t mainCarouselCount() {
    return MAIN_PAGE_COUNT;
}

void showProbeResults() {
    jumpToPage(UiPage::PROBE);
    menu.close();
}

void showSweepResults() {
    jumpToPage(UiPage::SWEEP);
    menu.close();
}

void showCellResults() {
    jumpToPage(UiPage::CELL);
    menu.close();
}

// Field Analyzer (Phase 10) — same "closes any open menu onto a specific
// page" shape as the three above. jumpToPage() itself is what triggers a
// first Scope capture (maybeStartScopeAcquire()); showScopePage() doesn't
// need its own copy of that logic.
void showMeterPage() {
    jumpToPage(UiPage::METER);
    menu.close();
}

void showWaterfallPage() {
    jumpToPage(UiPage::WATERFALL);
    menu.close();
}

void showScopePage() {
    jumpToPage(UiPage::SCOPE);
    menu.close();
}

void showCapturesPage() {
    jumpToPage(UiPage::CAPTURES);
    menu.close();
}

void showNodesPage() {
    jumpToPage(UiPage::NODES);
    menu.close();
}

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

    BaseType_t ok = xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 1, nullptr, 0);
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
