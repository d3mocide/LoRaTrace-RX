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

#include "energy_observation.h"

struct SweepMarginSettings {
    int16_t margin_dbm_x10 = ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10;
};

bool loadSweepMarginSettingsFromSD(SweepMarginSettings &settings);
bool writeSweepMarginSettingsToSD(const SweepMarginSettings &settings);
