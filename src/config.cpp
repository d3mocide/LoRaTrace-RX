#include "config.h"

#include <Arduino.h>
#include <SD.h>
#include <stdlib.h> // strtol, for the hex-or-decimal sync_word parse below
#include <string.h> // strlen, for the meshtastic_/meshcore_ key-prefix strip below

#include "detection.h" // missionProfileName(), for writeProfileConfigToSD()'s confirmation line
#include "serial_lock.h"
#include "spi_bus.h"
#include "file_transaction.h"
#include "channel_config.h"

// docs/DESIGN.md §1: module front end tuned 868-923MHz, 923-928 still in US ISM
// but reduced sensitivity — accept up to 928, RadioLib/the radio itself
// will reject anything the SX1262 truly can't do. Not file-static: wifi_task
// validates against these same bounds before ever writing to SD, so there is
// exactly one copy of each number rather than two that could drift apart.
bool channelFreqInRange(float mhz) { return mhz >= 868.0f && mhz <= 928.0f; }
bool channelSfInRange(uint8_t sf) { return sf >= 5 && sf <= 12; }
bool channelCrInRange(uint8_t cr) { return cr >= 5 && cr <= 8; }

namespace {

bool readConfigLocked(ProfileOverrides &out) {
    recoverTextFile(SD, CHANNEL_CONFIG_PATH);
    File f = SD.open(CHANNEL_CONFIG_PATH);
    if (!f) return false;
    out = ProfileOverrides{};
    while (f.available()) {
        char line[192];
        if (readBoundedLine(f, line, sizeof(line))) channelApplyConfigLine(line, out);
    }
    f.close();
    return true;
}

bool writeFullConfig(const ProfileOverrides &overrides) {
    const ChannelParams mt = resolvedChannelForProfile(overrides, MissionProfile::MESHTASTIC);
    const ChannelParams mc = resolvedChannelForProfile(overrides, MissionProfile::MESHCORE);
    char text[512];
    const int n = snprintf(text, sizeof(text),
        "meshtastic_freq_mhz=%.6f\nmeshtastic_sf=%u\nmeshtastic_bw_khz=%.2f\n"
        "meshtastic_cr_denom=%u\nmeshtastic_sync_word=%u\n"
        "meshcore_freq_mhz=%.6f\nmeshcore_sf=%u\nmeshcore_bw_khz=%.2f\n"
        "meshcore_cr_denom=%u\nmeshcore_sync_word=%u\n",
        (double)mt.freq_mhz, mt.sf, (double)mt.bw_khz, mt.cr_denom, mt.sync_word,
        (double)mc.freq_mhz, mc.sf, (double)mc.bw_khz, mc.cr_denom, mc.sync_word);
    return n > 0 && (size_t)n < sizeof(text) && replaceTextFile(SD, CHANNEL_CONFIG_PATH, text, n);
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
    recoverTextFile(SD, CHANNEL_CONFIG_PATH);
    if (!SD.exists(CHANNEL_CONFIG_PATH)) {
        if (writeFullConfig(overrides)) {
            Serial.print(F("[config] Created default "));
            Serial.print(CHANNEL_CONFIG_PATH);
            Serial.println(F(" — using built-in default channels."));
        }
        return false; // file we just wrote matches `overrides` already — nothing to apply
    }

    return readConfigLocked(overrides);
}

bool readProfileConfigFromSD(ProfileOverrides &out) {
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    return lock.held() && readConfigLocked(out);
}

bool writeProfileConfigToSD(MissionProfile profile, const ChannelParams &params,
                            const ProfileOverrides &current) {
    if ((profile != MissionProfile::MESHTASTIC && profile != MissionProfile::MESHCORE) ||
        !channelConfigValid(params)) return false;

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
    if (!readConfigLocked(updated)) return false;
    if (profile == MissionProfile::MESHCORE) {
        updated.meshcore = params;
        updated.meshcore_set = true;
    } else {
        updated.meshtastic = params;
        updated.meshtastic_set = true;
    }

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
