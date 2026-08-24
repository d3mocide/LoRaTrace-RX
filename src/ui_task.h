#pragma once
// LoRaTrace RX — UI task (Core 0).
//
// PHASE-ORDER NOTE: read-only status pages arrived in Phase 2, pulled
// forward from wherever CLAUDE.md originally proposed real UI, because
// field-testing for hours means reading the device *without* a tethered
// laptop — which the boot splash alone couldn't support. Phase 3/4 each
// added one binary keyboard gesture on top (WiFi toggle, profile switch) as
// narrow exceptions specifically because this project had no sourced
// Cardputer-ADV row/col-to-character keymap yet, so nothing beyond
// undifferentiated press/release timing was trustworthy.
//
// Phase 5 replaces both of those gestures with a real menu, now that
// keyboard.h has a sourced (partly bench-verified — see its own comments
// and PROGRESS.md's Phase 5 checklist) decode for eleven specific keys:
// ',' or ';' / '.' or '/' move (the Fn-arrow diamond's left/up and
// down/right, both pairs alias the same action), Enter selects, the
// backtick/ESC key goes back, '1'-'5' jump straight to a numbered carousel
// page. Modes built from those keys throughout:
//   - **Carousel** (default): ','/';'/'.'/'/' cycle the read-only status
//     pages below; digits '1'-'5' jump straight to one of them; Enter opens
//     the menu; the backtick/ESC key does nothing (nowhere to go back to).
//   - **Menu**: the same move keys move a highlighted selection; Enter
//     activates it (profile switch or WiFi toggle — the same
//     radio_task.h/wifi_task.h calls the old gestures made); the
//     backtick/ESC key returns to the carousel. Digit keys are ignored here
//     — the menu has its own two-item selection.
// Deliberately not a general keymap or text-entry UI — see keyboard.h for
// why eleven keys is enough for this scope (DESIGN.md/PROGRESS.md Phase 5).
//
// Owns the ST7789 exclusively once started — main.cpp must stop drawing.
// The display is on its own SPI host (HSPI) with pins disjoint from the
// radio/SD bus, so it needs no spi_bus arbitration.

#include <Arduino_GFX_Library.h>

// Pages the operator can cycle through in carousel mode. Order is
// deliberate: RADIO first because it's the reason the device exists,
// CHANNEL right after it since it's RADIO's own RF detail (the actual
// active freq/SF/BW/CR/sync word — added Phase 5, previously visible only
// over Serial or the web UI), GPS/SYSTEM/WIFI unchanged after.
enum class UiPage : uint8_t {
    RADIO = 0,
    CHANNEL,
    GPS,
    SYSTEM,
    WIFI,
    COUNT,
};

// Starts the task on Core 0. `gfx` must already be initialised (main.cpp
// brings it up for the boot splash). Returns false if the task could not be
// created; the caller should carry on regardless — a headless wardriver
// still logs, which is the actual job.
bool uiTaskStart(Arduino_GFX *gfx);

// True if the TCA8418 keyboard controller came up. When false the UI falls
// back to auto-advancing pages on a timer, so the device stays useful
// rather than stuck on one page.
bool uiKeyboardReady();
