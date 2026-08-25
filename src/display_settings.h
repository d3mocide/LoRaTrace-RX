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

struct DisplaySettings {
    // 5-100, 5% steps (ui_task.cpp's Brightness slider bounds).
    uint8_t brightness_pct = 100;
    // Index into ui_task.cpp's IDLE_TIMEOUT_OPTIONS (0 = Off); default 2
    // matches this feature's original hardcoded 60s default.
    uint8_t idle_timeout_index = 2;
};

bool loadDisplaySettingsFromSD(DisplaySettings &settings);
bool writeDisplaySettingsToSD(const DisplaySettings &settings);
