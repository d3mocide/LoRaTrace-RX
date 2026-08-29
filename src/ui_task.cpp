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
#include "keyboard.h"
#include "serial_control.h"
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
// Deliberately not branded per-profile (BRAND.md) — these are LoRa presets
// on one sniffer, not sibling products. Plain file scope (external
// linkage) because ui_pages.cpp's drawMenuList() identity-compares
// against it directly.
const MenuItem PROFILE_GROUP_ITEMS[] = {
    {"Meshtastic", ItemKind::ACTION, MenuAction::SELECT_MESHTASTIC, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"MeshCore", ItemKind::ACTION, MenuAction::SELECT_MESHCORE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
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
constexpr MenuItem SYSTEM_GROUP_ITEMS[] = {
    {"Connectivity", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, CONNECTIVITY_GROUP_ITEMS, 2},
    {"Diagnostics", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, DIAGNOSTICS_GROUP_ITEMS, 2},
    {"Display", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, DISPLAY_GROUP_ITEMS, 2},
};
// Trace remains the root-level operating toggle. Probe has no duplicate menu
// row: P is the global start/cancel shortcut and the dedicated second card
// also exposes it through Enter. Label has no baked-in colon -- drawMenuRow
// (ui_pages.cpp) inserts ": " itself whenever a row has a non-empty value,
// so every toggle-style row (Trace/Profile/WiFi/Debug/...) gets the same
// "Name: STATE" shape from one place instead of some labels remembering
// their own colon and others not (2026-08-28 operator request).
constexpr MenuItem ROOT_ITEMS[] = {
    {"Trace", ItemKind::ACTION, MenuAction::TRACE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Profile", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, PROFILE_GROUP_ITEMS, 2},
    {"System", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 3},
};
constexpr uint8_t ROOT_COUNT = 3;

// RX activity pulse: a brief, event-driven flash on the header's third
// status dot and a matching flash bar on RADIO, replacing an old idle
// heartbeat blink that only proved the UI task was alive, not that
// anything was being heard. Binary hold-then-revert, not an alpha-blended
// decay — RGB565 has no cheap alpha blending. File-local: only this file
// sets the deadline (on a new detection, in uiTask() below); ui_pages.cpp
// only reads rxPulseActive().
uint32_t rxPulseUntil = 0;
constexpr uint32_t RX_PULSE_MS = 220;

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

// No fillScreen() here: drawPage() (ui_pages.cpp) already wipes and
// redraws the whole content region every call, and the caller always
// follows a page change with fullRedraw() in the same loop iteration. An
// explicit clear here was a redundant second full-panel blank — the direct
// cause of a visible black flash on every page change (2026-08-25 bench).
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

// Drains the TCA8418 event FIFO and returns the most recently recognized
// KeyAction this poll (keyboard.h), or NONE. Several actions queued
// between polls collapse to the last one — acceptable at a 30ms poll
// interval for sparse, deliberate keypresses.
KeyAction pollKeyAction() {
    if (!keyboardReady) return KeyAction::NONE;
    KeyAction result = KeyAction::NONE;
    while (keys.available() > 0) {
        const KeyAction a = keyboardDecodeEvent((uint8_t)keys.getEvent());
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

        // Same async-completion-toast shape as Probe's block above, for Sweep.
        const uint32_t energyRuns = radioEnergySweepCount();
        if (energyRuns != lastEnergyRunSeen) {
            lastEnergyRunSeen = energyRuns;
            const bool energyFailed = radioEnergyFailureCount() != lastEnergyFailureSeen;
            const bool energyCancelled = radioEnergyCancelCount() != lastEnergyCancelSeen;
            lastEnergyFailureSeen = radioEnergyFailureCount();
            lastEnergyCancelSeen = radioEnergyCancelCount();
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
            sweepTerminalShownAt = millis();
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
            // state. showSweepResults() closes any open menu onto the
            // Sweep page after an accepted request, same as Probe.
            fireMenuAction(MenuAction::SWEEP_TOGGLE);
            redraw = true;
        } else if (!menu.isOpen()) {
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
                jumpToPage(UiPage::PROBE);
                redraw = true;
            } else if (action == KeyAction::JUMP_3) {
                jumpToPage(UiPage::SWEEP);
                redraw = true;
            } else if (action == KeyAction::JUMP_4) {
                jumpToPage(UiPage::CHANNEL);
                redraw = true;
            } else if (action == KeyAction::JUMP_5) {
                jumpToPage(UiPage::GPS);
                redraw = true;
            } else if (action == KeyAction::JUMP_6) {
                jumpToPage(UiPage::SYSTEM);
                redraw = true;
            } else if (action == KeyAction::SELECT && page == UiPage::RADIO) {
                fireMenuAction(MenuAction::TRACE_TOGGLE);
                redraw = true;
            } else if (action == KeyAction::SELECT && page == UiPage::PROBE) {
                fireMenuAction(MenuAction::PROBE_TOGGLE);
                redraw = true;
            } else if (action == KeyAction::SELECT && page == UiPage::SWEEP) {
                fireMenuAction(MenuAction::SWEEP_TOGGLE);
                redraw = true;
            }
            // Enter acts on RADIO (Trace), PROBE, and SWEEP (start/cancel).
            // Elsewhere it remains a no-op; ESC (BACK) opens the menu.
        } else if (action != KeyAction::NONE) {
            // Menu open (root/group/slider) — MenuState owns navigation;
            // this file only reacts to what fired. Captured before handle()
            // runs: leaving the Brightness slider (BACK, SLIDER -> ROOT) is
            // the debounce point for persisting it (see BRIGHTNESS_UP/DOWN
            // in ui_actions.cpp for why saves don't happen every step).
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

void showProbeResults() {
    jumpToPage(UiPage::PROBE);
    menu.close();
}

void showSweepResults() {
    jumpToPage(UiPage::SWEEP);
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
