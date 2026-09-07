#include "region_settings.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"
#include "file_transaction.h"

namespace {

constexpr const char *REGION_CONFIG_DIR = "/loratrace";
constexpr const char *REGION_CONFIG_PATH = "/loratrace/region.txt";


bool writeRegionConfigFile(const RegionSettings &settings) {
    char text[160];
    const int n = snprintf(text, sizeof(text), "region=%s\n", settings.region == Region::US ? "US" : "GLOBAL");
    return n > 0 && (size_t)n < sizeof(text) && replaceTextFile(SD, REGION_CONFIG_PATH, text, (size_t)n);
}

} // namespace

bool loadRegionSettingsFromSD(RegionSettings &settings) {
    // Does NOT call SD.begin() — main.cpp's earlier
    // loadProfileOverridesFromSD() call already mounted the card by the
    // time this runs (or there's no card at all, in which case these SD
    // calls fail safe and return false, same as an uninitialized SD object
    // always does).
    if (!SD.exists(REGION_CONFIG_DIR)) {
        SD.mkdir(REGION_CONFIG_DIR);
    }
    recoverTextFile(SD, REGION_CONFIG_PATH);
    if (!SD.exists(REGION_CONFIG_PATH)) {
        // First card seen by this firmware for the region setting — write
        // the current (struct-default) state so there's something to
        // see/edit next time, same convention loadDisplaySettingsFromSD()
        // already uses for display.txt.
        writeRegionConfigFile(settings);
        return false; // file we just wrote matches `settings` already
    }

    File f = SD.open(REGION_CONFIG_PATH);
    if (!f) return false;

    bool appliedAny = false;
    while (f.available()) {
        char line[192];
        if (!readBoundedLine(f, line, sizeof(line))) continue;
        if (applyRegionConfigLine(line, settings)) appliedAny = true;
    }
    f.close();
    return appliedAny;
}

bool writeRegionSettingsToSD(const RegionSettings &settings) {
    // Runtime call from ui_task (cycling System > Region) — same bounded
    // 2000ms wait writeDisplaySettingsToSD() uses, not portMAX_DELAY: an
    // operator-triggered save isn't worth stalling indefinitely for.
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) return false;

    return writeRegionConfigFile(settings);
}
