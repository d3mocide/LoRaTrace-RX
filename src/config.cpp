#include "config.h"

#include <Arduino.h>
#include <SD.h>
#include <stdlib.h> // strtol, for the hex-or-decimal sync_word parse below
#include <string.h> // strlen, for the meshtastic_/meshcore_ key-prefix strip below

#include "detection.h" // missionProfileName(), for writeProfileConfigToSD()'s confirmation line
#include "serial_lock.h"
#include "spi_bus.h"

// DESIGN.md §1: module front end tuned 868-923MHz, 923-928 still in US ISM
// but reduced sensitivity — accept up to 928, RadioLib/the radio itself
// will reject anything the SX1262 truly can't do. Not file-static: wifi_task
// validates against these same bounds before ever writing to SD, so there is
// exactly one copy of each number rather than two that could drift apart.
bool channelFreqInRange(float mhz) { return mhz >= 868.0f && mhz <= 928.0f; }
bool channelSfInRange(uint8_t sf) { return sf >= 5 && sf <= 12; }
bool channelCrInRange(uint8_t cr) { return cr >= 5 && cr <= 8; }

namespace {

// Applies one "key=value" line to whichever of `overrides`'s two profile
// slots the key's prefix names, setting that slot's `_set` flag when a
// field lands. Rejects recognized-but-out-of-range values with a warning
// rather than silently accepting them — a bad frequency or SF means the
// radio tunes to the wrong place and hears nothing.
bool applyConfigLine(const String &rawLine, ProfileOverrides &overrides) {
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

    // Prefix picks which profile this line belongs to — everything else
    // (bounds, parsing) is identical between the two, so the actual field
    // logic below is written once against a `ChannelParams&`/`bool&` pair
    // rather than duplicated per profile.
    ChannelParams *params;
    bool *setFlag;
    String field;
    if (key.startsWith("meshtastic_")) {
        params = &overrides.meshtastic;
        setFlag = &overrides.meshtastic_set;
        field = key.substring(strlen("meshtastic_"));
    } else if (key.startsWith("meshcore_")) {
        params = &overrides.meshcore;
        setFlag = &overrides.meshcore_set;
        field = key.substring(strlen("meshcore_"));
    } else {
        Serial.print(F("[config] unrecognized key '"));
        Serial.print(key);
        Serial.println(F("', ignoring (expected a meshtastic_ or meshcore_ prefix)."));
        return false;
    }

    if (field == "freq_mhz") {
        float v = val.toFloat();
        if (!channelFreqInRange(v)) {
            Serial.print(F("[config] "));
            Serial.print(key);
            Serial.print(F(" "));
            Serial.print(v, 3);
            Serial.println(F(" outside 868-928MHz, ignoring this field."));
            return false;
        }
        params->freq_mhz = v;
        *setFlag = true;
        return true;
    }
    if (field == "sf") {
        long v = val.toInt();
        if (v < 1 || !channelSfInRange((uint8_t)v)) {
            Serial.print(F("[config] "));
            Serial.print(key);
            Serial.print(F(" "));
            Serial.print(v);
            Serial.println(F(" outside 5-12, ignoring this field."));
            return false;
        }
        params->sf = (uint8_t)v;
        *setFlag = true;
        return true;
    }
    if (field == "bw_khz") {
        float v = val.toFloat();
        if (v <= 0.0f) {
            Serial.print(F("[config] "));
            Serial.print(key);
            Serial.println(F(" must be positive, ignoring this field."));
            return false;
        }
        params->bw_khz = v;
        *setFlag = true;
        return true;
    }
    if (field == "cr_denom") {
        long v = val.toInt();
        if (v < 1 || !channelCrInRange((uint8_t)v)) {
            Serial.print(F("[config] "));
            Serial.print(key);
            Serial.print(F(" "));
            Serial.print(v);
            Serial.println(F(" outside 5-8, ignoring this field."));
            return false;
        }
        params->cr_denom = (uint8_t)v;
        *setFlag = true;
        return true;
    }
    // Accepts hex ("0x2B") or decimal ("43") — strtol base 0 picks by
    // prefix. Overridable from SD so a sync word can be A/B tested on the
    // bench without a reflash.
    if (field == "sync_word") {
        char *end = nullptr;
        long v = strtol(val.c_str(), &end, 0);
        if (end == val.c_str() || *end != '\0' || v < 0 || v > 0xFF) {
            Serial.print(F("[config] "));
            Serial.print(key);
            Serial.print(F(" '"));
            Serial.print(val);
            Serial.println(F("' is not a byte in 0x00-0xFF, ignoring this field."));
            return false;
        }
        params->sync_word = (uint8_t)v;
        *setFlag = true;
        return true;
    }

    Serial.print(F("[config] unrecognized key '"));
    Serial.print(key);
    Serial.println(F("', ignoring."));
    return false;
}

// Writes one profile's five key=value lines under `prefix`, resolved to
// its loaded override or its hardcoded default — the file always ends up
// with a complete, valid block for both profiles.
void writeProfileBlock(File &f, const char *prefix, MissionProfile profile,
                       const ProfileOverrides &overrides) {
    const ChannelParams p = resolvedChannelForProfile(overrides, profile);
    f.print(prefix);
    f.print(F("freq_mhz="));
    f.println(p.freq_mhz, 3);
    f.print(prefix);
    f.print(F("sf="));
    f.println(p.sf);
    f.print(prefix);
    f.print(F("bw_khz="));
    f.println(p.bw_khz, 1);
    f.print(prefix);
    f.print(F("cr_denom="));
    f.println(p.cr_denom);
    f.print(prefix);
    f.print(F("sync_word=0x"));
    f.println(p.sync_word, HEX);
}

// Writes `overrides` out to CHANNEL_CONFIG_PATH — both profiles' blocks,
// each resolved to its override if set, else its hardcoded default — as an
// active (uncommented) config, valid and loadable from the moment it's
// created. Assumes CHANNEL_CONFIG_DIR already exists. Returns false
// (logging why) if the SD card won't accept the write.
bool writeFullConfig(const ProfileOverrides &overrides) {
    File f = SD.open(CHANNEL_CONFIG_PATH, FILE_WRITE);
    if (!f) {
        Serial.print(F("[config] Could not create "));
        Serial.print(CHANNEL_CONFIG_PATH);
        Serial.println(F(" (SD may be read-only) — using built-in default channels."));
        return false;
    }

    f.println(F("# LoRaTrace RX — channel config"));
    f.println(F("#"));
    f.println(F("# One block per mission profile (meshtastic_*/meshcore_*) — each is"));
    f.println(F("# independent, so a custom Meshtastic frequency and MeshCore's stock"));
    f.println(F("# default (or vice versa) can coexist, and switching profiles on-device"));
    f.println(F("# no longer drops whichever one you're not currently on. Auto-created on"));
    f.println(F("# first boot with an SD card present, set to both profiles' built-in"));
    f.println(F("# defaults. Edit the values below for your local mesh (e.g. a regional"));
    f.println(F("# preset like MeshOregon) — check your own node's radio config, don't"));
    f.println(F("# guess. Lines starting with # are ignored. An out-of-range value is"));
    f.println(F("# rejected with a warning on serial and skipped, not applied — it won't"));
    f.println(F("# brick the boot."));
    f.println();
    f.println(F("# Meshtastic preset"));
    writeProfileBlock(f, "meshtastic_", MissionProfile::MESHTASTIC, overrides);
    f.println();
    f.println(F("# MeshCore preset"));
    writeProfileBlock(f, "meshcore_", MissionProfile::MESHCORE, overrides);
    f.close();
    return true;
}

} // namespace

// This function, applyConfigLine(), and writeFullConfig() are only ever
// called once from main.cpp's setup(), before any task exists, so none of
// their Serial prints take serial_lock.h's lock. writeProfileConfigToSD()
// below is the *runtime* entry point (wifi_task's settings save, called
// with every other task live) and does take it.
bool loadProfileOverridesFromSD(ProfileOverrides &overrides, int8_t csPin, SPIClass &spi,
                                 bool *sdMounted) {
    const bool mounted = SD.begin(csPin, spi);
    if (sdMounted != nullptr) *sdMounted = mounted;
    if (!mounted) {
        Serial.println(F("[config] No SD card detected (or mount failed) — using built-in default channels."));
        return false;
    }

    // First card ever seen by this firmware: create /loratrace/config.txt
    // with both profiles' current (hardcoded, since `overrides` is still
    // fresh) defaults, so operators have a file to edit in place instead of
    // hand-copying sd-template/loratrace/.
    if (!SD.exists(CHANNEL_CONFIG_DIR)) {
        SD.mkdir(CHANNEL_CONFIG_DIR);
    }
    if (!SD.exists(CHANNEL_CONFIG_PATH)) {
        if (writeFullConfig(overrides)) {
            Serial.print(F("[config] Created default "));
            Serial.print(CHANNEL_CONFIG_PATH);
            Serial.println(F(" — using built-in default channels."));
        }
        return false; // file we just wrote matches `overrides` already — nothing to apply
    }

    File f = SD.open(CHANNEL_CONFIG_PATH);
    if (!f) {
        Serial.print(F("[config] "));
        Serial.print(CHANNEL_CONFIG_PATH);
        Serial.println(F(" not found — using built-in default channels."));
        return false;
    }

    bool appliedAny = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (applyConfigLine(line, overrides)) appliedAny = true;
    }
    f.close();

    if (appliedAny) {
        Serial.print(F("[config] Applied channel override(s) from "));
        Serial.println(CHANNEL_CONFIG_PATH);
    } else {
        Serial.print(F("[config] "));
        Serial.print(CHANNEL_CONFIG_PATH);
        Serial.println(F(" found but had no valid keys — using built-in default channels."));
    }
    return appliedAny;
}

bool writeProfileConfigToSD(MissionProfile profile, const ChannelParams &params,
                            const ProfileOverrides &current) {
    if (!channelFreqInRange(params.freq_mhz) || !channelSfInRange(params.sf) ||
        !channelCrInRange(params.cr_denom) || params.bw_khz <= 0.0f) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) Serial.println(F("[config] refusing to write out-of-range channel params."));
        return false;
    }

    // Runtime call, unlike loadProfileOverridesFromSD's boot-time one —
    // the radio/GPS/logger tasks are all live and sharing this SPI bus, so
    // this must arbitrate for it. Bounded wait, not portMAX_DELAY: an
    // operator-triggered save isn't worth stalling indefinitely for.
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) {
        SerialLock slock(pdMS_TO_TICKS(200));
        if (slock.held()) Serial.println(F("[config] could not get the SPI bus to write channel config."));
        return false;
    }

    // Start from `current` and overlay just the one profile being saved —
    // the other profile's block is rewritten unchanged, so saving one
    // preset can never clobber the other.
    ProfileOverrides updated = current;
    if (profile == MissionProfile::MESHCORE) {
        updated.meshcore = params;
        updated.meshcore_set = true;
    } else {
        updated.meshtastic = params;
        updated.meshtastic_set = true;
    }

    // Delete-then-recreate rather than truncate-in-place: a shorter new
    // config must not leave trailing bytes of the old file behind.
    // writeFullConfig() already writes the exact key=value format
    // loadProfileOverridesFromSD() parses — reused as-is.
    SD.remove(CHANNEL_CONFIG_PATH);
    const bool ok = writeFullConfig(updated);

    // A save from the web UI has no other on-device confirmation — the
    // browser shows its own message, but an operator with a serial console
    // open sees nothing without this. One buffer, printed under the Serial
    // lock (an earlier unlocked version of this exact line came out torn
    // on hardware — see serial_lock.h).
    char line[160]; // worst case ("meshtastic", 3-digit fields) measures ~111B
    if (ok) {
        snprintf(line, sizeof(line),
                 "[config] Wrote %s (%s): %.3fMHz SF%u BW%.1f CR4/%u sync 0x%X — reboot to apply.",
                 CHANNEL_CONFIG_PATH, missionProfileName((uint8_t)profile), (double)params.freq_mhz,
                 (unsigned)params.sf, (double)params.bw_khz, (unsigned)params.cr_denom,
                 (unsigned)params.sync_word);
    } else {
        snprintf(line, sizeof(line), "[config] Failed to write %s (SD busy, missing, or read-only).",
                 CHANNEL_CONFIG_PATH);
    }
    {
        SerialLock slock(pdMS_TO_TICKS(200));
        if (slock.held()) serialPrintln(line);
    }
    return ok;
}
