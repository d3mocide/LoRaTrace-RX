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

// Writes `defaults` out to CHANNEL_CONFIG_PATH as an active (uncommented)
// config, so the file is a valid, loadable config in its own right from the
// moment it's created — not just a commented-out template the operator has
// to uncomment first. Assumes CHANNEL_CONFIG_DIR already exists. Returns
// false (logging why) if the SD card won't accept the write, e.g. read-only.
bool writeDefaultConfig(const ChannelParams &defaults) {
    File f = SD.open(CHANNEL_CONFIG_PATH, FILE_WRITE);
    if (!f) {
        Serial.print(F("[config] Could not create "));
        Serial.print(CHANNEL_CONFIG_PATH);
        Serial.println(F(" (SD may be read-only) — using built-in default channel."));
        return false;
    }

    f.println(F("# LoRaTrace RX — channel config"));
    f.println(F("#"));
    f.println(F("# Auto-created on first boot with an SD card present, set to the"));
    f.println(F("# built-in Meshtastic LongFast (US) default. Edit the values below for"));
    f.println(F("# your local mesh (e.g. a regional preset like MeshOregon) — check your"));
    f.println(F("# own node's radio config, don't guess. Lines starting with # are"));
    f.println(F("# ignored. An out-of-range value is rejected with a warning on serial"));
    f.println(F("# and skipped, not applied — it won't brick the boot."));
    f.println();
    f.print(F("freq_mhz="));
    f.println(defaults.freq_mhz, 3);
    f.print(F("sf="));
    f.println(defaults.sf);
    f.print(F("bw_khz="));
    f.println(defaults.bw_khz, 1);
    f.print(F("cr_denom="));
    f.println(defaults.cr_denom);
    f.close();
    return true;
}

} // namespace

bool loadChannelConfigFromSD(ChannelParams &params, int8_t csPin, SPIClass &spi) {
    if (!SD.begin(csPin, spi)) {
        Serial.println(F("[config] No SD card detected (or mount failed) — using built-in default channel."));
        return false;
    }

    // First card ever seen by this firmware: create /loratrace/config.txt
    // with the current defaults so operators have a file to edit in place,
    // instead of needing to hand-copy sd-template/loratrace/ themselves.
    if (!SD.exists(CHANNEL_CONFIG_DIR)) {
        SD.mkdir(CHANNEL_CONFIG_DIR);
    }
    if (!SD.exists(CHANNEL_CONFIG_PATH)) {
        if (writeDefaultConfig(params)) {
            Serial.print(F("[config] Created default "));
            Serial.print(CHANNEL_CONFIG_PATH);
            Serial.println(F(" — using built-in default channel."));
        }
        return false; // file we just wrote matches `params` already — nothing to apply
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
