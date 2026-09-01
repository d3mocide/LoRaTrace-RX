#include "region_settings.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"

namespace {

constexpr const char *REGION_CONFIG_DIR = "/loratrace";
constexpr const char *REGION_CONFIG_PATH = "/loratrace/region.txt";

bool applyRegionConfigLine(const String &rawLine, RegionSettings &settings) {
    String line = rawLine;
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) return false;

    int eq = line.indexOf('=');
    if (eq < 0) return false;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();
    if (key.length() == 0 || val.length() == 0) return false;

    if (key == "region") {
        if (val == "US") {
            settings.region = Region::US;
            return true;
        }
        if (val == "GLOBAL") {
            settings.region = Region::GLOBAL;
            return true;
        }
        return false;
    }
    return false;
}

// Delete-then-recreate, not truncate-in-place — same reasoning
// writeProfileConfigToSD()/writeDisplayConfigFile() document: a shorter
// new file must not leave a trailing byte of the old one behind.
bool writeRegionConfigFile(const RegionSettings &settings) {
    SD.remove(REGION_CONFIG_PATH);
    File f = SD.open(REGION_CONFIG_PATH, FILE_WRITE);
    if (!f) return false;

    f.println(F("# LoRaTrace RX — region setting"));
    f.println(F("# region: US or GLOBAL (constrains Sweep's scanned band)"));
    f.print(F("region="));
    f.println(settings.region == Region::US ? "US" : "GLOBAL");
    f.close();
    return true;
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
        String line = f.readStringUntil('\n');
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
