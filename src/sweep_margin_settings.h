#pragma once
// LoRaTrace RX — Sweep peak-detection margin setting, persisted to SD,
// separate from region_settings.h's band-constraint scope and
// display_settings.h's display scope on purpose.
//
// Same small-single-purpose-module convention as display_settings.h/.cpp
// and region_settings.h/.cpp: its own file rather than growing an existing
// one past its stated scope.
//
// Mirrors region_settings.h/.cpp's load/write pair exactly:
//   - loadSweepMarginSettingsFromSD() is boot-time, called once from
//     main.cpp's setup() right after loadRegionSettingsFromSD() (which has
//     already mounted the card by then — this does NOT call SD.begin()
//     itself).
//   - writeSweepMarginSettingsToSD() is the runtime entry point (ui_task.cpp,
//     leaving the System > Tuning > Margin slider), same bounded-wait SD
//     arbitration writeRegionSettingsToSD() uses.
//
// File: /loratrace/sweep_margin.txt, sibling to config.txt/display.txt/
// region.txt. Fails safe the same way those do: missing card/file/bad
// values leave `settings` at its struct default
// (ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10, the calibrated bench default),
// never a partial/garbage state.
//
// See energy_observation.h's own comment on
// ENERGY_SWEEP_MARGIN_MIN_DBM_X10/MAX for why this became operator-
// adjustable (docs/STATUS.md's 2026-09-03 "Sweep silence" investigation).

#include <stdint.h>
#include <string.h>

#include "config_line.h"

#include "energy_observation.h"

struct SweepMarginSettings {
    int16_t margin_dbm_x10 = ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10;
};

// Pure and host-tested — see applyCaptureConfigLine()'s note in
// capture_settings.h for why these live in the headers.
inline bool applySweepMarginConfigLine(const char *rawLine, SweepMarginSettings &settings) {
    char key[32];
    char value[32];
    if (!configLineSplit(rawLine, key, sizeof(key), value, sizeof(value))) return false;

    if (strcmp(key, "margin_dbm_x10") == 0) {
        long parsed = 0;
        if (!configParseLongInRange(value, ENERGY_SWEEP_MARGIN_MIN_DBM_X10,
                                    ENERGY_SWEEP_MARGIN_MAX_DBM_X10, parsed)) {
            return false;
        }
        settings.margin_dbm_x10 = (int16_t)parsed;
        return true;
    }
    return false;
}

bool loadSweepMarginSettingsFromSD(SweepMarginSettings &settings);
bool writeSweepMarginSettingsToSD(const SweepMarginSettings &settings);
