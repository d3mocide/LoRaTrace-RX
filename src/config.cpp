#include "config.h"

#include <Arduino.h>
#include <SD.h>

namespace {

// DESIGN.md §1: module front end tuned 868-923MHz, 923-928 still in US ISM
// but reduced sensitivity — accept up to 928, RadioLib/the radio itself
// will reject anything the SX1262 truly can't do.
bool freqInRange(float mhz) { return mhz >= 868.0f && mhz <= 928.0f; }
bool sfInRange(uint8_t sf) { return sf >= 5 && sf <= 12; }
bool crInRange(uint8_t cr) { return cr >= 5 && cr <= 8; }

// Applies one "key=value" line to `params`. Returns true if a recognized,
// in-range key was applied. Rejects recognized-but-out-of-range values
// with a warning instead of silently accepting them — a bad frequency or
// SF here means the radio tunes to the wrong place and hears nothing,
// which is exactly the failure mode this whole project exists to avoid.
bool applyConfigLine(const String &rawLine, ChannelParams &params) {
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

    if (key == "freq_mhz") {
        float v = val.toFloat();
        if (!freqInRange(v)) {
            Serial.print(F("[config] freq_mhz "));
            Serial.print(v, 3);
            Serial.println(F(" outside 868-928MHz, ignoring this field."));
            return false;
        }
        params.freq_mhz = v;
        return true;
    }
    if (key == "sf") {
        long v = val.toInt();
        if (v < 1 || !sfInRange((uint8_t)v)) {
            Serial.print(F("[config] sf "));
            Serial.print(v);
            Serial.println(F(" outside 5-12, ignoring this field."));
            return false;
        }
        params.sf = (uint8_t)v;
        return true;
    }
    if (key == "bw_khz") {
        float v = val.toFloat();
        if (v <= 0.0f) {
            Serial.println(F("[config] bw_khz must be positive, ignoring this field."));
            return false;
        }
        params.bw_khz = v;
        return true;
    }
    if (key == "cr_denom") {
        long v = val.toInt();
        if (v < 1 || !crInRange((uint8_t)v)) {
            Serial.print(F("[config] cr_denom "));
            Serial.print(v);
            Serial.println(F(" outside 5-8, ignoring this field."));
            return false;
        }
        params.cr_denom = (uint8_t)v;
        return true;
    }

    Serial.print(F("[config] unrecognized key '"));
    Serial.print(key);
    Serial.println(F("', ignoring."));
    return false;
}

} // namespace

bool loadChannelConfigFromSD(ChannelParams &params, int8_t csPin, SPIClass &spi) {
    if (!SD.begin(csPin, spi)) {
        Serial.println(F("[config] No SD card detected (or mount failed) — using built-in default channel."));
        return false;
    }

    File f = SD.open(CHANNEL_CONFIG_PATH);
    if (!f) {
        Serial.print(F("[config] "));
        Serial.print(CHANNEL_CONFIG_PATH);
        Serial.println(F(" not found — using built-in default channel."));
        return false;
    }

    bool appliedAny = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (applyConfigLine(line, params)) appliedAny = true;
    }
    f.close();

    if (appliedAny) {
        Serial.print(F("[config] Applied channel override from "));
        Serial.println(CHANNEL_CONFIG_PATH);
    } else {
        Serial.print(F("[config] "));
        Serial.print(CHANNEL_CONFIG_PATH);
        Serial.println(F(" found but had no valid keys — using built-in default channel."));
    }
    return appliedAny;
}
