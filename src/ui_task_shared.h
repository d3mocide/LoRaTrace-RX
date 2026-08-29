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
// PROGRESS.md/CLAUDE.md) into three files by concern — drawing
// (ui_pages.cpp), menu-action business logic (ui_actions.cpp), and task
// lifecycle/input/main loop (ui_task.cpp, which also owns every symbol
// declared `extern` below) — no behavior change intended, only file
// boundaries. Everything here that ISN'T declared extern (the constants)
// stays a plain header-local `constexpr`, same as before the split — no
// ODR risk, each including TU just gets its own identical copy.

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
constexpr uint8_t BRIGHTNESS_MIN = 5;
constexpr uint8_t BRIGHTNESS_MAX = 100;
constexpr uint8_t BRIGHTNESS_STEP = 5;
struct IdleTimeoutOption {
    const char *label;
    uint32_t ms; // unused when index 0 ("Off")
};
extern const IdleTimeoutOption IDLE_TIMEOUT_OPTIONS[];
constexpr uint8_t IDLE_TIMEOUT_OPTION_COUNT = 5;

// --- Cross-file entry points ---
void drawHeader();                      // ui_pages.cpp  — called by ui_task.cpp's fullRedraw()
void drawPage();                        // ui_pages.cpp  — called by ui_task.cpp's fullRedraw()
void fireMenuAction(MenuAction action); // ui_actions.cpp — called by ui_task.cpp's main loop
void showProbeResults();                // ui_task.cpp — closes menu onto the Probe page
void showSweepResults();                // ui_task.cpp — closes menu onto the Sweep page
