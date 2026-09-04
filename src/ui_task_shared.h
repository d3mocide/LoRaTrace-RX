#pragma once
// LoRaTrace RX — internal state and helpers shared across ui_task's own
// split implementation files (ui_task.cpp / ui_pages.cpp / ui_actions.cpp).
//
// NOT a public task interface. main.cpp and every other task only ever see
// ui_task.h's two functions (uiTaskStart()/uiKeyboardReady()) — this header
// exists purely so the three files below, which together implement one
// cohesive on-device UI, can share operator-facing state (active page, open
// menu, brightness/idle-dim, toast/RX-pulse feedback) and the draw target
// without duplicating it three ways or reaching around through ui_task.h.
//
// Split out of a single ~1265-line ui_task.cpp (2026-08-25 cleanup pass,
// docs/history/CHANGELOG.md/CLAUDE.md) into three files by concern — drawing
// (ui_pages.cpp), menu-action business logic (ui_actions.cpp), and task
// lifecycle/input/main loop (ui_task.cpp, which also owns every symbol
// declared `extern` below) — no behavior change intended, only file
// boundaries. Everything here that ISN'T declared extern (the constants)
// stays a plain header-local `constexpr`, same as before the split — no
// ODR risk, each including TU just gets its own identical copy.

#include "display_settings.h" // BRIGHTNESS_MIN/MAX/STEP, IDLE_TIMEOUT_INDEX_MAX
#include "ui_menu.h"
#include "ui_task.h" // UiPage, Arduino_GFX (transitively)

// --- Draw target ---
// Allocated by ui_task.cpp's uiTaskStart() (the off-screen
// Arduino_Canvas_Indexed, or a panel fallback if that allocation fails —
// see its own comment there for why). Every drawing function in
// ui_pages.cpp targets this, exactly as before the split.
extern Arduino_GFX *uiTft;

// --- Keyboard presence ---
// Set once in uiTaskStart(); read by ui_task.cpp's pollKeyAction()/main
// loop and by ui_pages.cpp's drawSystemPage() ("keys tca8418"/"none
// (auto)" line).
extern bool keyboardReady;

// --- Carousel state ---
// nextPage()/prevPage()/jumpToPage() stay ui_task.cpp's own concern
// (carousel navigation is this subsystem's job, not ui_menu.h's
// MenuState's) — `page` is exposed here only so ui_pages.cpp's
// drawPage()/drawFooterStatus() can read the current page.
extern UiPage page;
// 1-based position/total for drawFooterStatus()'s "N/M" — the operator-
// facing main carousel (Radio/Tools/Analyze/Channel/GPS/System), not raw
// UiPage ordinals. See mainCarouselPosition()'s own comment (ui_task.cpp)
// for why that distinction matters.
uint8_t mainCarouselPosition();
uint8_t mainCarouselCount();

// --- Menu ---
// The grouped root/group/slider MenuState instance and its backing tables
// are defined in ui_task.cpp. PROFILE_GROUP_ITEMS is exposed separately
// (not reachable only through `menu`) because ui_pages.cpp's
// drawMenuList() identity-compares a list's `items` pointer against it to
// special-case the "Profile: <live value>" row label.
extern MenuState menu;
extern const MenuItem PROFILE_GROUP_ITEMS[];

// --- Toast overlay ---
// A brief feedback message, shown independent of whichever page/menu
// level is on screen. showToast()/toastActive() remain the only way to
// set/query it from other files; ui_pages.cpp's drawToast() reads the raw
// fields directly for its slide/countdown-bar animation math, same direct
// access it had before the split.
extern char toastMsg[48];
extern uint32_t toastShownAt;
void showToast(const char *msg);
bool toastActive();
constexpr uint32_t TOAST_DURATION_MS = 1400;

// --- RX activity pulse ---
// The timestamp itself (rxPulseUntil) stays file-local to ui_task.cpp,
// which is the only place that ever sets it (on a newly-observed
// detection in the main loop) — ui_pages.cpp only ever needs to ask
// whether it's currently active, never the raw deadline.
bool rxPulseActive();

// --- Brightness / idle-dim ---
// The operator's live settings. Read directly by ui_pages.cpp's menu/
// slider drawing, mutated directly by ui_actions.cpp's fireMenuAction()
// (BRIGHTNESS_UP/DOWN, IDLE_TIMEOUT_CYCLE); ui_task.cpp owns seeding them
// from SD at boot, the idle-dim timing loop, and persisting them back.
extern uint8_t activeBrightnessPercent;
extern bool displayDimmed;
extern uint8_t idleTimeoutIndex;
// BRIGHTNESS_MIN/MAX/STEP now come from display_settings.h — the module
// that persists them and validates them on the way back in — so the slider
// and the file parser cannot drift apart.
struct IdleTimeoutOption {
    const char *label;
    uint32_t ms; // unused when index 0 ("Off")
};
extern const IdleTimeoutOption IDLE_TIMEOUT_OPTIONS[];
constexpr uint8_t IDLE_TIMEOUT_OPTION_COUNT = 5;
// The table here and the persisted index bound in display_settings.h are
// the same fact stated twice; this is what keeps them honest.
static_assert(IDLE_TIMEOUT_OPTION_COUNT == IDLE_TIMEOUT_INDEX_MAX + 1,
              "IDLE_TIMEOUT_OPTIONS and display_settings.h's persisted index bound disagree");

// --- Probe/Sweep result hold ---
// Timestamp of the first redraw after a NEW terminal Probe/Sweep result
// (set by ui_task.cpp's existing async-completion polling, the same place
// that already fires the toast for it), or 0 before any run this
// power-on. drawProbePage()/drawSweepPage() (ui_pages.cpp) use this to
// revert the big state word to a dim "IDLE" after RESULT_HOLD_MS, while
// leaving the rest of the card showing the last real result — an
// operator-visible "ready to run again" cue distinct from a genuine
// never-run state, requested after watching the on-device behavior.
extern uint32_t probeTerminalShownAt;
extern uint32_t sweepTerminalShownAt;
// Same idle-reversion cue for Cell's own card (Phase 11, 2026-09-01).
extern uint32_t cellTerminalShownAt;
// Same idle-reversion cue for Field Analyzer's Scope card (Phase 10).
extern uint32_t scopeTerminalShownAt;
constexpr uint32_t RESULT_HOLD_MS = 8000;

// --- Cross-file entry points ---
void drawHeader();                      // ui_pages.cpp  — called by ui_task.cpp's fullRedraw()
void drawPage();                        // ui_pages.cpp  — called by ui_task.cpp's fullRedraw()
void fireMenuAction(MenuAction action); // ui_actions.cpp — called by ui_task.cpp's main loop
void showProbeResults();                // ui_task.cpp — closes menu onto the Probe page
void showSweepResults();                // ui_task.cpp — closes menu onto the Sweep page
void showCellResults();                 // ui_task.cpp — closes menu onto the Cell page
// Field Analyzer (Phase 10) — same "closes any open menu onto a specific
// page" shape as the three above, fired by the Analyze hub's own row
// selection (ui_task.cpp) via fireMenuAction() (ui_actions.cpp).
// showToolsPage()/showAnalyzePage() themselves were removed 2026-09-04 —
// see ROOT_ITEMS's own comment (ui_task.cpp) for why the root menu no
// longer needs a shortcut to either hub.
void showMeterPage();
void showWaterfallPage();
void showScopePage();
void showCapturesPage();
void showNodesPage();

// --- Analyze hub ---
// The row currently highlighted on the Analyze hub page (0..
// ANALYZE_HUB_COUNT-1, Meter/Waterfall/Scope/Captures/Nodes in that order).
// ui_task.cpp owns moving it (PREV/NEXT while on UiPage::ANALYZE) and
// firing its SELECT action; ui_pages.cpp's drawAnalyzePage() only reads it
// to decide which row to invert.
extern uint8_t analyzeHubIndex;
constexpr uint8_t ANALYZE_HUB_COUNT = 5;

// --- Tools hub ---
// Same contract as the Analyze hub above, for UiPage::TOOLS's own three
// rows (Probe/Sweep/Cell, in that order).
extern uint8_t toolsHubIndex;
constexpr uint8_t TOOLS_HUB_COUNT = 3;
