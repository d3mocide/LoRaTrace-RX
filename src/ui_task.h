#pragma once
// LoRaTrace RX — UI task (Core 0).
//
// PHASE-ORDER NOTE: CLAUDE.md puts real UI in Phase 6, after the discovery
// engines. This arrives early at the operator's explicit request, because
// field-testing Phase 2 for hours means reading the device *without* a
// tethered laptop — which the boot splash alone can't support. Recorded in
// PROGRESS.md's decisions log, same as the earlier splash exception.
//
// Scope is deliberately narrow even so: multiple read-only status pages, a
// battery indicator, and (Phase 3/4) two binary keyboard gestures. Still no
// menus and no configuration editing — those need more than a duration
// threshold on an undifferentiated keypress to do well, and would bake in
// keymap assumptions this project hasn't earned (no sourced Cardputer-ADV
// row/col-to-character map — see pollKeyGesture()'s comment in ui_task.cpp).
//
// Two narrow exceptions built entirely from press/release timing, needing no
// keymap at all: a ~1.2s hold toggles the WiFi AP (Phase 3, wifi_task.h),
// and a ~3s hold (Phase 4) requests the mission-profile switch DESIGN.md §5
// describes — Meshtastic <-> MeshCore, mutually exclusive, radio_task.h's
// radioRequestProfileSwitch(). Both are single binary gestures layered on
// the same duration check that already turns a tap into "next page" — there
// is still no "which key" to get wrong, because neither gesture asks.
//
// Owns the ST7789 exclusively once started — main.cpp must stop drawing.
// The display is on its own SPI host (HSPI) with pins disjoint from the
// radio/SD bus, so it needs no spi_bus arbitration.

#include <Arduino_GFX_Library.h>

// Pages the operator can cycle through. Order is deliberate: RADIO first
// because it's the reason the device exists, SYSTEM last because it only
// matters when something is wrong.
enum class UiPage : uint8_t {
    RADIO = 0,
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
