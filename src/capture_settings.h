#pragma once
// LoRaTrace RX — repeat-mode Sweep capture-window setting, persisted to SD.
//
// Same small-single-purpose-module convention as display_settings.h,
// region_settings.h and sweep_margin_settings.h: its own file rather than
// growing an existing one past its stated scope (sweep_margin_settings.h's
// scope is the peak-detection margin, not this).
//
// Mirrors those three exactly: loadCaptureSettingsFromSD() is boot-time
// (main.cpp, after the card is already mounted — it does NOT call
// SD.begin()); writeCaptureSettingsToSD() is the runtime entry point
// (ui_task.cpp, cycling System > Tuning > Capture) and arbitrates
// spi_bus.h's mutex itself with the same bounded wait.
//
// File: /loratrace/capture.txt, sibling to config.txt/display.txt/
// region.txt/sweep_margin.txt. Fails safe the same way: missing card/file/
// bad value leaves `settings` at its struct default.
//
// What it controls: how long repeat-mode Sweep parks on the home channel
// with RX armed between laps, servicing real packets
// (radio_task.cpp's performEnergySweepHomeListen()). This is a direct
// trade — survey cadence against packet capture — which is why it's an
// operator choice rather than a fixed constant. Measured on real traffic
// (docs/STATUS.md, v1.0.3): OFF captured 0 of 42 packets (0.0%), 2s
// captured 22 of 27 (81.5%) with the full-band survey slowing from ~0.9s
// to ~2.9s per lap.

#include <stdint.h>
#include <string.h>

#include "config_line.h"

// Index into the table below rather than raw ms, so a corrupt file can
// only ever select a real option — same reasoning as DisplaySettings'
// idle_timeout_index.
constexpr uint32_t CAPTURE_WINDOW_OPTIONS_MS[] = {0, 1000, 2000, 4000};
constexpr const char *CAPTURE_WINDOW_LABELS[] = {"OFF", "1s", "2s", "4s"};
constexpr uint8_t CAPTURE_WINDOW_OPTION_COUNT = 4;
// 2000ms — the value the v1.0.3 A/B actually validated at 81.5% capture.
constexpr uint8_t CAPTURE_WINDOW_DEFAULT_INDEX = 2;

struct CaptureSettings {
    uint8_t window_index = CAPTURE_WINDOW_DEFAULT_INDEX;
};

inline uint32_t captureWindowMsForIndex(uint8_t index) {
    if (index >= CAPTURE_WINDOW_OPTION_COUNT) index = CAPTURE_WINDOW_DEFAULT_INDEX;
    return CAPTURE_WINDOW_OPTIONS_MS[index];
}

inline const char *captureWindowLabelForIndex(uint8_t index) {
    if (index >= CAPTURE_WINDOW_OPTION_COUNT) index = CAPTURE_WINDOW_DEFAULT_INDEX;
    return CAPTURE_WINDOW_LABELS[index];
}

// Pure, so it is host-testable (test/test_settings_parse/) rather than only
// reachable by booting hardware with a real card. Returns true only when a
// line actually applied a value; a blank/comment/unknown/out-of-range line
// returns false and leaves `settings` alone, so a damaged file degrades to
// the struct defaults instead of a half-applied mix.
inline bool applyCaptureConfigLine(const char *rawLine, CaptureSettings &settings) {
    char key[32];
    char value[32];
    if (!configLineSplit(rawLine, key, sizeof(key), value, sizeof(value))) return false;

    if (strcmp(key, "window_index") == 0) {
        long parsed = 0;
        if (!configParseLongInRange(value, 0, CAPTURE_WINDOW_OPTION_COUNT - 1, parsed)) return false;
        settings.window_index = (uint8_t)parsed;
        return true;
    }
    return false;
}

bool loadCaptureSettingsFromSD(CaptureSettings &settings);
bool writeCaptureSettingsToSD(const CaptureSettings &settings);
