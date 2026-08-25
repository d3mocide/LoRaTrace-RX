// LoRaTrace RX — ui_task lifecycle, input, and main loop.
//
// Split out of a single ~1265-line ui_task.cpp (2026-08-25 cleanup pass,
// PROGRESS.md/CLAUDE.md) into three files by concern: this file (task
// lifecycle, keyboard input decode, the main loop, and every piece of
// operator-facing state — carousel page, menu, toast, RX pulse,
// brightness/idle-dim — since it owns seeding that state at boot and
// persisting it back to SD), drawing (ui_pages.cpp), and menu-action
// business logic (ui_actions.cpp). See ui_task.h for the subsystem's
// overall design and ui_task_shared.h for the internal contract between
// these three files. No behavior change intended, only file boundaries.

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
#include "radio_task.h"

// --- Shared state (extern-declared in ui_task_shared.h) ---
// Defined here; this file is the single definition point for every symbol
// declared extern there. Deliberately at plain file scope (not inside the
// anonymous namespace below) so it gets the external linkage ui_pages.cpp/
// ui_actions.cpp need to see it.

// uiTft is the draw target every drawing function writes to (ui_pages.cpp,
// plus this file's own fullRedraw()). As of the Phase 6 bench pass
// (2026-08-25) it points at an off-screen Arduino_Canvas_Indexed buffer,
// not the physical panel directly — see uiTaskStart() below for why.
Arduino_GFX *uiTft = nullptr;

// CRITICAL: the TCA8418 boots in SLEEP and reports nothing until explicitly
// configured, even when I2C is perfectly healthy. Exactly the failure shape
// as the GPS power rail (a working bus proving nothing about a working
// device). begin() + matrix() is the wake sequence, taken from
// bmorcelli/Launcher's confirmed-working Cardputer-ADV interface.
bool keyboardReady = false;

UiPage page = UiPage::RADIO;

// Toast layer (Phase 6): a brief overlay message for feedback that isn't
// tied to whichever menu row happens to be highlighted — e.g. confirming a
// toggle fired right before BACK leaves the menu. Only this static buffer,
// not a dynamic allocation — no heap number to gate behind the way WiFi's
// AP needed one.
char toastMsg[48] = {0};
uint32_t toastShownAt = 0;

// Brightness + idle-dim (2026-08-25, second revision — slider + persisted
// settings). activeBrightnessPercent is the operator's chosen level (5-100,
// BRIGHTNESS_STEP at a time) — what idle-dim restores to on the next
// keypress, not necessarily what the backlight is driven at right now
// (which may be a lower idle floor while displayDimmed — see
// idleDimTargetPercent() below). Seeded from SD (uiTaskStart()'s
// `settings` param) instead of a hardcoded default, so the device
// remembers what an operator picked across power cycles, same as channel
// overrides already do.
uint8_t activeBrightnessPercent = 100;
bool displayDimmed = false;

// Idle-dim timeout, cycled from System's "Idle dim" group item
// (IDLE_TIMEOUT_CYCLE) instead of a plain on/off — index 0 is "Off"
// (disables idle-dim entirely, matching what the old AUTODIM_TOGGLE=false
// used to mean), the rest are real durations. Index 2 (60s) is the
// default, matching this feature's original hardcoded value. Also seeded
// from SD.
const IdleTimeoutOption IDLE_TIMEOUT_OPTIONS[] = {
    {"Off", 0},
    {"30s", 30000},
    {"60s", 60000},
    {"2min", 120000},
    {"5min", 300000},
};
uint8_t idleTimeoutIndex = 2;

// Phase 6 root table, revised four times 2026-08-25 (BRAND.md's "Revised
// again" note has the Profile/System history). "Profile" opens onto the
// real, technical profile names — Meshtastic, MeshCore today; Reticulum/
// Spectrum join once they have a real HOME_LISTEN table (Phase 8) — instead
// of cycling one at a time on Enter the way Phase 4/5 did. Deliberately not
// branded as "Mesh Trace" or similar: these are LoRa presets on one
// sniffer, not sibling products, and BRAND.md already had "Profile" as its
// preferred word for exactly this axis before an earlier same-day revision
// briefly tried a per-profile brand name instead. Defined at plain file
// scope (external linkage, not the anonymous namespace below) because
// ui_pages.cpp's drawMenuList() identity-compares against it directly.
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
// different address, which is ordinary shared-bus operation.
Adafruit_TCA8418 keys;

uint32_t lastPageChange = 0;

// Display (2026-08-25, third revision): Brightness/idle-dim moved out of
// System's own flat list into their own nested group on operator request
// ("system > display > display relevant settings") — the first thing in
// this project to actually need ui_menu.h's generalized nesting (System's
// own WiFi/Debug/Trace grouping was flat, one level, from the start; this
// is a GROUP inside a GROUP). Brightness stays a SLIDER row, just reached
// one level deeper than when it briefly lived at root.
constexpr MenuItem DISPLAY_GROUP_ITEMS[] = {
    {"Brightness", ItemKind::SLIDER, MenuAction::NONE, MenuAction::BRIGHTNESS_UP, MenuAction::BRIGHTNESS_DOWN, nullptr, 0},
    // Idle-dim timeout: cycles Off/30s/60s/2min/5min on each Enter press,
    // same "fires and stays in the list" shape WiFi/Debug already have,
    // just cycling a value instead of flipping a bool — an operator asked
    // for real choices here instead of a hardcoded 60s.
    {"Idle dim", ItemKind::ACTION, MenuAction::IDLE_TIMEOUT_CYCLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};
constexpr MenuItem SYSTEM_GROUP_ITEMS[] = {
    {"WiFi", ItemKind::ACTION, MenuAction::WIFI_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Debug", ItemKind::ACTION, MenuAction::DEBUG_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Display", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, DISPLAY_GROUP_ITEMS, 2},
};
// Trace pause/standby (2026-08-25) is a root-level ACTION row, not a System
// item — promoted the same day it shipped, on operator feedback that it's
// central enough to toggle without drilling into System first. Deliberately
// named "Trace" alone, not "MeshTrace": that exact compound word was walked
// back earlier this same session (BRAND.md's Interface Naming table) for
// overloading "Trace" across the product name, a per-profile brand, and a
// saved-session noun — reviving it here for a fourth meaning (live radio
// state) would repeat the same mistake. Puts the SX1262 in its warm sleep
// mode instead of continuous RX — a real battery lever, unlike GPS
// (io_expander.h: GPS power shares the antenna-switch line, so there's no
// independent GPS power to save, and this deliberately leaves GPS running
// so position is already fresh the instant Trace resumes).
constexpr MenuItem ROOT_ITEMS[] = {
    {"Trace", ItemKind::ACTION, MenuAction::TRACE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Profile", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, PROFILE_GROUP_ITEMS, 2},
    {"System", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 3},
};
constexpr uint8_t ROOT_COUNT = 3;

// RX activity pulse (Phase 6 UI redesign): a brief, event-driven flash on
// the header's third status dot and a matching flash line on RADIO,
// replacing the old idle heartbeat blink — that only ever proved the UI
// task's own loop was alive, not that anything was actually being heard.
// Binary hold-then-revert, not the alpha-blended decay the design mockup
// used: Arduino_GFX/RGB565 has no cheap alpha blending, so "recently
// active" is a solid color held for RX_PULSE_MS and then reverted, rather
// than a fading glow. File-local: only this file ever sets the deadline
// (on a newly-observed detection, in uiTask() below); ui_pages.cpp only
// ever asks rxPulseActive() whether it's still active.
uint32_t rxPulseUntil = 0;
constexpr uint32_t RX_PULSE_MS = 220;

// The level idle-dim actually drives when it engages: the lower of a fixed
// floor and the operator's own active level. Needed once brightness became
// a slider that can go below the old fixed IDLE_DIM_PERCENT (15) — without
// this, setting an active level of e.g. 10% and then going idle would make
// the screen get BRIGHTER (jump to 15%) instead of dimmer.
constexpr uint8_t IDLE_DIM_FLOOR = 15;
uint8_t idleDimTargetPercent() {
    return activeBrightnessPercent < IDLE_DIM_FLOOR ? activeBrightnessPercent : IDLE_DIM_FLOOR;
}

// lastKeyActivity tracks any recognized KeyAction, the same basis
// AUTO_ADVANCE_MS's carousel-timer already uses for "idle" — on a
// keyboardless unit (!keyboardReady) this never advances past boot, so the
// display dims at the configured timeout and stays dimmed for the rest of
// an unattended run. That's the right outcome for exactly the multi-hour-
// unattended-drive scenario this project is built around, not a corner
// case to special-case away.
uint32_t lastKeyActivity = 0;

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

// No fillScreen() here: drawPage() (ui_pages.cpp) already unconditionally
// wipes the whole content region and redraws every element on every call
// (it has to, since pages don't track their own prior state), and the
// caller always follows a page change with a fullRedraw() in the same loop
// iteration (uiTask() below). An explicit clear here was a second
// full-panel blank stacked right before drawPage()'s own — pure redundant
// SPI traffic, and the direct cause of the visible black flash on every
// page change (bench feedback, 2026-08-25 hardware pass).
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

// NOTE: no startWrite()/endWrite() batching here on purpose, despite
// looking like the obvious next step. Arduino_GFX's fillRect()/print()
// etc. already each wrap themselves in their own startWrite()/endWrite()
// pair, and Arduino_HWSPI's is a straight SPIClass::beginTransaction()/
// endTransaction() call with a plain, non-recursive lock — a second,
// outer startWrite() around a sequence of calls that each take it again
// internally deadlocks on the first nested call (verified against the
// vendored GFX/SPI sources, not run on hardware — caught before it became
// a hang on first boot). That's moot now anyway: uiTft->flush() below is the
// real single-transaction boundary (Arduino_TFT::drawIndexedBitmap — one
// startWrite()/writeIndexedPixels()/endWrite() sequence over the whole
// composed frame), and it isn't nested inside anything.
void fullRedraw() {
    drawHeader();
    drawPage();
    uiTft->flush();
}

void uiTask(void *) {
    fullRedraw();

    uint32_t lastRedraw = millis();
    lastPageChange = lastRedraw;
    lastKeyActivity = lastRedraw;
    uint32_t lastRxSeen = radioPacketCount();
    bool wasAnimating = false;

    for (;;) {
        const KeyAction action = pollKeyAction();
        bool redraw = false;

        // Brightness idle-dim: any recognized key both resets the idle
        // clock and immediately undims if the display was dimmed — an
        // operator who just pressed something can't still be "idle" the
        // instant after. Checked before the carousel/menu dispatch below
        // so a keypress that also does something else (page change, menu
        // action) still counts as activity. idleTimeoutIndex == 0 ("Off")
        // disables idle-dim entirely, same meaning the old AUTODIM_TOGGLE
        // == false had.
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
            // Menu open (root/group/slider level) — MenuState owns
            // navigation and level transitions; this file only reacts to
            // what fired. Captured before handle() runs: leaving the
            // Brightness slider (BACK, SLIDER -> ROOT) is the debounce
            // point for persisting it — see BRIGHTNESS_UP/DOWN's own
            // comment in ui_actions.cpp's fireMenuAction() for why saves
            // don't happen on every step instead.
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

        // Every tick below always goes through fullRedraw(), including the
        // FAST_REDRAW_MS animation burst — unlike direct-to-panel drawing,
        // redrawing "too much" into the off-screen canvas costs nothing the
        // viewer can see, since uiTft->flush() is the only point anything
        // reaches the glass, as one atomic blit. A leaner toast-only path
        // that skipped drawPage() briefly existed here before the canvas;
        // it existed purely to avoid a *visible* partial redraw, which
        // isn't a concern any more, so it was more code for no remaining
        // benefit.
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
            // The toast or RX pulse just expired since the last redraw —
            // force one more full pass so its overlay actually clears
            // rather than lingering until the next periodic REDRAW_MS tick
            // (up to ~1s stale).
            fullRedraw();
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

// menu's constructor needs ROOT_ITEMS/ROOT_COUNT (both file-local, defined
// just above) — fine to reference here even though this definition itself
// needs external linkage (ui_pages.cpp/ui_actions.cpp both use `menu`
// directly): linkage is a per-declaration property, not a scoping
// restriction, so an externally-linked global can still be initialized
// from an internally-linked one earlier in the same translation unit.
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

    // Seed from main.cpp's boot-time SD load (display_settings.h) instead
    // of this file's hardcoded defaults — clamped defensively even though
    // display_settings.cpp already validates on load, since these values
    // go straight into backlightSetPercent() below. A brand-new/empty SD
    // card leaves `settings` at its own struct defaults (100%, 60s), which
    // is the same effective behavior this feature originally shipped with.
    activeBrightnessPercent = settings.brightness_pct;
    if (activeBrightnessPercent < BRIGHTNESS_MIN) activeBrightnessPercent = BRIGHTNESS_MIN;
    if (activeBrightnessPercent > BRIGHTNESS_MAX) activeBrightnessPercent = BRIGHTNESS_MAX;
    idleTimeoutIndex = settings.idle_timeout_index;
    if (idleTimeoutIndex >= IDLE_TIMEOUT_OPTION_COUNT) idleTimeoutIndex = 2;
    // main.cpp's backlightInit() (called before this, for the boot splash)
    // always starts at 100% — apply the real loaded level now so it's
    // visible from ui_task's very first frame instead of staying at 100%
    // until the operator happens to touch the Brightness slider.
    backlightSetPercent(activeBrightnessPercent);

    // Phase 6 bench pass (2026-08-25) found direct-to-panel drawing causes
    // real, visible flicker and tearing: every fillRect()/print() call is
    // immediately visible on the glass, so any redraw shows a blank-then-
    // redrawn flash to the eye even for values that didn't actually change,
    // and the toast's fast-redraw burst tore for the same reason. Fixed
    // the way M5PORKCHOP-style M5GFX/LovyanGFX sprite UIs get their
    // smoothness: everything above draws into this off-screen canvas
    // instead of the panel, and nothing reaches the glass until
    // uiTft->flush() blits the whole composed frame in one shot
    // (Arduino_TFT::drawIndexedBitmap — a single startWrite()/
    // writeIndexedPixels()/endWrite() sequence, confirmed against the
    // vendored GFX source). _Indexed rather than the full RGB565 canvas:
    // this UI only ever uses 6 colours (COL_BG/FG/DIM/GOOD/WARN/BAD, all
    // in ui_pages.cpp), so 1 byte/pixel loses nothing and costs ~32KB
    // (240*135) instead of RGB565's ~63KB (240*135*2) — half the resource
    // commitment for a UI that never needed more than 6 distinct colours.
    // No PSRAM on this board (DESIGN.md §1), so this is a real malloc()
    // against the same heap budget as everything else, not a static
    // allocation — a deliberate one-time ~32KB tradeoff, decided with the
    // operator rather than assumed, given this reverses what was
    // previously a deliberate "no canvas/framebuffer" choice. Falls back
    // to drawing straight on the panel (the original behavior, flicker and
    // all) if the allocation fails, rather than taking the whole UI down
    // over ~32KB of missing heap headroom.
    canvas = new Arduino_Canvas_Indexed(gfx->width(), gfx->height(), gfx, 0, 0, 0);
    if (canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        uiTft = canvas;
    } else {
        delete canvas;
        canvas = nullptr;
        uiTft = gfx;
    }

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
