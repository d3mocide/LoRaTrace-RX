#include "sweep_margin_settings.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"

namespace {

constexpr const char *SWEEP_MARGIN_CONFIG_DIR = "/loratrace";
constexpr const char *SWEEP_MARGIN_CONFIG_PATH = "/loratrace/sweep_margin.txt";


// Delete-then-recreate, not truncate-in-place — same reasoning
// writeRegionConfigFile()/writeDisplayConfigFile() document: a shorter new
// file must not leave a trailing byte of the old one behind.
bool writeSweepMarginConfigFile(const SweepMarginSettings &settings) {
    SD.remove(SWEEP_MARGIN_CONFIG_PATH);
    File f = SD.open(SWEEP_MARGIN_CONFIG_PATH, FILE_WRITE);
    if (!f) return false;

    f.println(F("# LoRaTrace RX — Sweep peak-detection margin"));
    f.println(F("# margin_dbm_x10: tenths of dB above the rolling noise floor"));
    f.print(F("margin_dbm_x10="));
    f.println(settings.margin_dbm_x10);
    f.close();
    return true;
}

} // namespace

bool loadSweepMarginSettingsFromSD(SweepMarginSettings &settings) {
    // Does NOT call SD.begin() — main.cpp's earlier
    // loadProfileOverridesFromSD() call already mounted the card by the
    // time this runs (or there's no card at all, in which case these SD
    // calls fail safe and return false, same as an uninitialized SD object
    // always does).
    if (!SD.exists(SWEEP_MARGIN_CONFIG_DIR)) {
        SD.mkdir(SWEEP_MARGIN_CONFIG_DIR);
    }
    if (!SD.exists(SWEEP_MARGIN_CONFIG_PATH)) {
        // First card seen by this firmware for the margin setting — write
        // the current (struct-default) state so there's something to
        // see/edit next time, same convention loadRegionSettingsFromSD()
        // already uses for region.txt.
        writeSweepMarginConfigFile(settings);
        return false; // file we just wrote matches `settings` already
    }

    File f = SD.open(SWEEP_MARGIN_CONFIG_PATH);
    if (!f) return false;

    bool appliedAny = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (applySweepMarginConfigLine(line.c_str(), settings)) appliedAny = true;
    }
    f.close();
    return appliedAny;
}

bool writeSweepMarginSettingsToSD(const SweepMarginSettings &settings) {
    if (settings.margin_dbm_x10 < ENERGY_SWEEP_MARGIN_MIN_DBM_X10 ||
        settings.margin_dbm_x10 > ENERGY_SWEEP_MARGIN_MAX_DBM_X10) {
        return false;
    }

    // Runtime call from ui_task (leaving the Margin slider) — same bounded
    // 2000ms wait writeRegionSettingsToSD() uses, not portMAX_DELAY: an
    // operator-triggered save isn't worth stalling indefinitely for.
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) return false;

    return writeSweepMarginConfigFile(settings);
}
