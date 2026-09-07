#include "display_settings.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"
#include "file_transaction.h"

namespace {

constexpr const char *DISPLAY_CONFIG_DIR = "/loratrace";
constexpr const char *DISPLAY_CONFIG_PATH = "/loratrace/display.txt";


bool writeDisplayConfigFile(const DisplaySettings &settings) {
    char text[160];
    const int n = snprintf(text, sizeof(text), "brightness_pct=%u\nidle_timeout_index=%u\n", (unsigned)settings.brightness_pct, (unsigned)settings.idle_timeout_index);
    return n > 0 && (size_t)n < sizeof(text) && replaceTextFile(SD, DISPLAY_CONFIG_PATH, text, (size_t)n);
}

} // namespace

bool loadDisplaySettingsFromSD(DisplaySettings &settings) {
    // Does NOT call SD.begin() — main.cpp's earlier
    // loadProfileOverridesFromSD() call already mounted the card by the
    // time this runs (or there's no card at all, in which case these SD
    // calls fail safe and return false, same as an uninitialized SD object
    // always does).
    if (!SD.exists(DISPLAY_CONFIG_DIR)) {
        SD.mkdir(DISPLAY_CONFIG_DIR);
    }
    recoverTextFile(SD, DISPLAY_CONFIG_PATH);
    if (!SD.exists(DISPLAY_CONFIG_PATH)) {
        // First card seen by this firmware for display settings — write
        // the current (struct-default) state so there's something to
        // see/edit next time, same convention
        // loadProfileOverridesFromSD() already uses for config.txt.
        writeDisplayConfigFile(settings);
        return false; // file we just wrote matches `settings` already
    }

    File f = SD.open(DISPLAY_CONFIG_PATH);
    if (!f) return false;

    bool appliedAny = false;
    while (f.available()) {
        char line[192];
        if (!readBoundedLine(f, line, sizeof(line))) continue;
        if (applyDisplayConfigLine(line, settings)) appliedAny = true;
    }
    f.close();
    return appliedAny;
}

bool writeDisplaySettingsToSD(const DisplaySettings &settings) {
    if (settings.brightness_pct < BRIGHTNESS_MIN || settings.brightness_pct > BRIGHTNESS_MAX ||
        settings.brightness_pct % 5 != 0 ||
        settings.idle_timeout_index > IDLE_TIMEOUT_INDEX_MAX) {
        return false;
    }

    // Runtime call from ui_task (its first-ever SD access) — same bounded
    // 2000ms wait writeProfileConfigToSD() uses, not portMAX_DELAY: an
    // operator-triggered save isn't worth stalling indefinitely for.
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) return false;

    return writeDisplayConfigFile(settings);
}
