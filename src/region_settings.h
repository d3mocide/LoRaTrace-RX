#pragma once
// LoRaTrace RX — region setting (Sweep's band constraint), persisted to SD,
// separate from config.h's channel-override scope and display_settings.h's
// display scope on purpose.
//
// Same small-single-purpose-module convention as display_settings.h/.cpp:
// its own file rather than growing an existing one past its stated scope.
//
// Mirrors display_settings.h/.cpp's load/write pair exactly:
//   - loadRegionSettingsFromSD() is boot-time, called once from main.cpp's
//     setup() right after loadDisplaySettingsFromSD() (which has already
//     mounted the card by then — this does NOT call SD.begin() itself).
//   - writeRegionSettingsToSD() is the runtime entry point (ui_task.cpp,
//     cycling System > Region), same bounded-wait SD arbitration
//     writeDisplaySettingsToSD() uses.
//
// File: /loratrace/region.txt, sibling to config.txt/display.txt. Fails
// safe the same way those do: missing card/file/bad values leave
// `settings` at its struct default (Region::US, operator-confirmed
// default), never a partial/garbage state.
//
// Region is currently consumed only by ENERGY_SWEEP's band selection
// (energy_plan.h) — Cell's FCC-specific framing and channel_plans.h's
// per-profile frequency tables are explicit, larger follow-ups
// (docs/ROADMAP.md), not wired to this yet.

#include <string.h>

#include "config_line.h"
#include "region_plan.h"

struct RegionSettings {
    Region region = Region::US;
};

// Pure and host-tested — see applyCaptureConfigLine()'s note in
// capture_settings.h. Region is a token, not a number, so it validates by
// exact match: an unrecognised token is rejected rather than defaulted, or
// a typo'd region would silently scan the wrong band.
inline bool applyRegionConfigLine(const char *rawLine, RegionSettings &settings) {
    char key[32];
    char value[32];
    if (!configLineSplit(rawLine, key, sizeof(key), value, sizeof(value))) return false;

    if (strcmp(key, "region") == 0) {
        if (strcmp(value, "US") == 0) {
            settings.region = Region::US;
            return true;
        }
        if (strcmp(value, "GLOBAL") == 0) {
            settings.region = Region::GLOBAL;
            return true;
        }
        return false;
    }
    return false;
}

bool loadRegionSettingsFromSD(RegionSettings &settings);
bool writeRegionSettingsToSD(const RegionSettings &settings);
