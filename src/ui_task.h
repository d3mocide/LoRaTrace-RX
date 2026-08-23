#pragma once
// LoRaTrace RX — UI task (Core 0).
//
// PHASE-ORDER NOTE: CLAUDE.md puts real UI in Phase 6, after the discovery
// engines. This arrives early at the operator's explicit request, because
// field-testing Phase 2 for hours means reading the device *without* a
// tethered laptop — which the boot splash alone can't support. Recorded in
// PROGRESS.md's decisions log, same as the earlier splash exception.
//
// Scope is deliberately narrow even so: multiple read-only status pages and
// a battery indicator. No menus, no configuration editing, no mission-profile
// switching. Those need the state machine that Phases 4/5 build, and adding
// them now would bake in assumptions this project hasn't earned yet.
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
