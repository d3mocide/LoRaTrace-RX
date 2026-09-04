#pragma once
// LoRaTrace RX — display settings (brightness, idle-dim timeout) persisted
// to SD, separate from config.h's channel-override scope on purpose.
//
// config.h's own header comment already states its scope as deliberately
// narrow ("not the general Logger/settings architecture Phase 2 will
// eventually own") — display settings are exactly what that carve-out was
// for. Same small-single-purpose-module convention as io_expander.h,
// spi_bus.h, serial_lock.h, battery.h, backlight.h: its own file rather
// than growing an existing one past its stated scope.
//
// Mirrors config.h/config.cpp's load/write pair exactly:
//   - loadDisplaySettingsFromSD() is boot-time, called once from main.cpp's
//     setup() right after loadProfileOverridesFromSD() (which has already
//     mounted the card by then — this does NOT call SD.begin() itself).
//   - writeDisplaySettingsToSD() is the runtime entry point (ui_task.cpp,
//     leaving the Brightness slider / cycling the idle timeout), and
//     arbitrates spi_bus.h's mutex itself, the same way
//     writeProfileConfigToSD() does for the web UI's settings save — this
//     is ui_task's first-ever SD access, so that discipline matters here
//     for the first time from this file.
//
// File: /loratrace/display.txt, sibling to config.txt. Fails safe the same
// way config.txt does: missing card/file/bad values leave `settings` at
// its struct defaults, never a partial/garbage state.

#include <stdint.h>
#include <string.h>

#include "config_line.h"

struct DisplaySettings {
    // 5-100, 5% steps (ui_task.cpp's Brightness slider bounds).
    uint8_t brightness_pct = 100;
    // Index into ui_task.cpp's IDLE_TIMEOUT_OPTIONS (0 = Off); default 2
    // matches this feature's original hardcoded 60s default.
    uint8_t idle_timeout_index = 2;
};

// The Brightness slider's bounds and step. These used to exist twice —
// once in display_settings.cpp for validation and once in ui_task_shared.h
// for the slider — with a comment on each explaining that duplicating two
// small numbers beat coupling the modules. That trade is no longer needed:
// this header is pure (no Arduino, no SD), so ui_task_shared.h simply
// includes it and the numbers have one definition. Moved up from the .cpp
// so the parser below can be host-tested.
constexpr uint8_t BRIGHTNESS_MIN = 5;
constexpr uint8_t BRIGHTNESS_MAX = 100;
constexpr uint8_t BRIGHTNESS_STEP = 5;
constexpr uint8_t IDLE_TIMEOUT_INDEX_MAX = 4; // Off/30s/60s/2min/5min = 0-4

// Pure and host-tested — see applyCaptureConfigLine()'s note in
// capture_settings.h.
inline bool applyDisplayConfigLine(const char *rawLine, DisplaySettings &settings) {
    char key[32];
    char value[32];
    if (!configLineSplit(rawLine, key, sizeof(key), value, sizeof(value))) return false;

    if (strcmp(key, "brightness_pct") == 0) {
        long parsed = 0;
        if (!configParseLongInRange(value, BRIGHTNESS_MIN, BRIGHTNESS_MAX, parsed)) return false;
        // The slider only ever produces multiples of its step, so a value
        // off the step grid is a hand-edit that the UI could not round-trip.
        if (parsed % BRIGHTNESS_STEP != 0) return false;
        settings.brightness_pct = (uint8_t)parsed;
        return true;
    }
    if (strcmp(key, "idle_timeout_index") == 0) {
        long parsed = 0;
        if (!configParseLongInRange(value, 0, IDLE_TIMEOUT_INDEX_MAX, parsed)) return false;
        settings.idle_timeout_index = (uint8_t)parsed;
        return true;
    }
    return false;
}

bool loadDisplaySettingsFromSD(DisplaySettings &settings);
bool writeDisplaySettingsToSD(const DisplaySettings &settings);
