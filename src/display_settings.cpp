#include "display_settings.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"

namespace {

constexpr const char *DISPLAY_CONFIG_DIR = "/loratrace";
constexpr const char *DISPLAY_CONFIG_PATH = "/loratrace/display.txt";

// Same bounds ui_task.cpp's Brightness slider / IDLE_TIMEOUT_OPTIONS use —
// duplicated here rather than shared, since pulling in ui_task.h just for
// two constants would be a much larger coupling than two small numbers
// agreeing by convention (same trade config.cpp's own channel bounds
// already accept against wifi_task's settings endpoint).
constexpr uint8_t BRIGHTNESS_MIN = 5;
constexpr uint8_t BRIGHTNESS_MAX = 100;
constexpr uint8_t IDLE_TIMEOUT_INDEX_MAX = 4; // Off/30s/60s/2min/5min = 0-4

bool applyDisplayConfigLine(const String &rawLine, DisplaySettings &settings) {
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

    if (key == "brightness_pct") {
        long v = val.toInt();
        if (v < BRIGHTNESS_MIN || v > BRIGHTNESS_MAX) return false;
        settings.brightness_pct = (uint8_t)v;
        return true;
    }
    if (key == "idle_timeout_index") {
        long v = val.toInt();
        if (v < 0 || v > IDLE_TIMEOUT_INDEX_MAX) return false;
        settings.idle_timeout_index = (uint8_t)v;
        return true;
    }
    return false;
}

// Delete-then-recreate, not truncate-in-place — same reasoning
// writeProfileConfigToSD() documents: a shorter new file must not leave a
// trailing byte of the old one behind.
bool writeDisplayConfigFile(const DisplaySettings &settings) {
    SD.remove(DISPLAY_CONFIG_PATH);
    File f = SD.open(DISPLAY_CONFIG_PATH, FILE_WRITE);
    if (!f) return false;

    f.println(F("# LoRaTrace RX — display settings"));
    f.println(F("# brightness_pct: 5-100 in 5% steps"));
    f.println(F("# idle_timeout_index: 0=Off 1=30s 2=60s 3=2min 4=5min"));
    f.print(F("brightness_pct="));
    f.println(settings.brightness_pct);
    f.print(F("idle_timeout_index="));
    f.println(settings.idle_timeout_index);
    f.close();
    return true;
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
        String line = f.readStringUntil('\n');
        if (applyDisplayConfigLine(line, settings)) appliedAny = true;
    }
    f.close();
    return appliedAny;
}

bool writeDisplaySettingsToSD(const DisplaySettings &settings) {
    if (settings.brightness_pct < BRIGHTNESS_MIN || settings.brightness_pct > BRIGHTNESS_MAX ||
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
