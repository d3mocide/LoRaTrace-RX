#include "capture_settings.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"

namespace {

constexpr const char *CAPTURE_CONFIG_DIR = "/loratrace";
constexpr const char *CAPTURE_CONFIG_PATH = "/loratrace/capture.txt";

bool applyCaptureConfigLine(const String &rawLine, CaptureSettings &settings) {
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

    if (key == "window_index") {
        long v = val.toInt();
        if (v < 0 || v >= CAPTURE_WINDOW_OPTION_COUNT) return false;
        settings.window_index = (uint8_t)v;
        return true;
    }
    return false;
}

// Delete-then-recreate, not truncate-in-place — same reasoning
// writeRegionConfigFile()/writeSweepMarginConfigFile() document.
bool writeCaptureConfigFile(const CaptureSettings &settings) {
    SD.remove(CAPTURE_CONFIG_PATH);
    File f = SD.open(CAPTURE_CONFIG_PATH, FILE_WRITE);
    if (!f) return false;

    f.println(F("# LoRaTrace RX — repeat-mode Sweep capture window"));
    f.println(F("# window_index: 0=Off 1=1s 2=2s 3=4s"));
    f.println(F("# How long repeat Sweep parks on the home channel between"));
    f.println(F("# laps to receive real packets. Trades survey cadence for capture."));
    f.print(F("window_index="));
    f.println(settings.window_index);
    f.close();
    return true;
}

} // namespace

bool loadCaptureSettingsFromSD(CaptureSettings &settings) {
    // Does NOT call SD.begin() — main.cpp's earlier
    // loadProfileOverridesFromSD() already mounted the card (or there is no
    // card, in which case these calls fail safe and return false).
    if (!SD.exists(CAPTURE_CONFIG_DIR)) {
        SD.mkdir(CAPTURE_CONFIG_DIR);
    }
    if (!SD.exists(CAPTURE_CONFIG_PATH)) {
        // First card seen by this firmware for the capture setting — write
        // the struct-default state so there's something to see/edit next
        // time, same convention loadRegionSettingsFromSD() already uses.
        writeCaptureConfigFile(settings);
        return false; // file we just wrote matches `settings` already
    }

    File f = SD.open(CAPTURE_CONFIG_PATH);
    if (!f) return false;

    bool appliedAny = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (applyCaptureConfigLine(line, settings)) appliedAny = true;
    }
    f.close();
    return appliedAny;
}

bool writeCaptureSettingsToSD(const CaptureSettings &settings) {
    if (settings.window_index >= CAPTURE_WINDOW_OPTION_COUNT) return false;

    // Runtime call from ui_task (cycling System > Tuning > Capture) — same
    // bounded 2000ms wait writeRegionSettingsToSD() uses, not portMAX_DELAY.
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) return false;

    return writeCaptureConfigFile(settings);
}
