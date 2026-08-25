#include "profile_state.h"

#include <Arduino.h>
#include <SD.h>

#include "spi_bus.h"

namespace {

constexpr const char *PROFILE_STATE_DIR = "/loratrace";
constexpr const char *PROFILE_STATE_PATH = "/loratrace/profile.txt";

// Only Meshtastic/MeshCore are reachable via the on-device menu's runtime
// switch (ui_actions.cpp) — Reticulum/General Exploration have no live
// switch path yet, so there's nothing else this file needs to round-trip.
const char *profileToken(MissionProfile profile) {
    return profile == MissionProfile::MESHCORE ? "meshcore" : "meshtastic";
}

bool tokenToProfile(const String &token, MissionProfile &profile) {
    if (token == "meshtastic") {
        profile = MissionProfile::MESHTASTIC;
        return true;
    }
    if (token == "meshcore") {
        profile = MissionProfile::MESHCORE;
        return true;
    }
    return false;
}

// Delete-then-recreate, not truncate-in-place — same reasoning
// writeDisplayConfigFile() documents: a shorter new file must not leave a
// trailing byte of the old one behind.
bool writeProfileStateFile(MissionProfile profile) {
    SD.remove(PROFILE_STATE_PATH);
    File f = SD.open(PROFILE_STATE_PATH, FILE_WRITE);
    if (!f) return false;

    f.println(F("# LoRaTrace RX — last active profile (menu-driven switches only)"));
    f.println(F("# active_profile: meshtastic or meshcore"));
    f.print(F("active_profile="));
    f.println(profileToken(profile));
    f.close();
    return true;
}

} // namespace

bool loadLastProfileFromSD(MissionProfile &profile) {
    // Does NOT call SD.begin() — main.cpp's earlier
    // loadProfileOverridesFromSD() call already mounted the card by the
    // time this runs (or there's no card at all, in which case these SD
    // calls fail safe and return false).
    if (!SD.exists(PROFILE_STATE_DIR)) {
        SD.mkdir(PROFILE_STATE_DIR);
    }
    if (!SD.exists(PROFILE_STATE_PATH)) {
        // First card seen by this firmware for profile state — write the
        // caller's current (default) profile so there's something to see
        // next time, same convention loadDisplaySettingsFromSD() uses.
        writeProfileStateFile(profile);
        return false; // file we just wrote matches `profile` already
    }

    File f = SD.open(PROFILE_STATE_PATH);
    if (!f) return false;

    bool applied = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        if (key != "active_profile") continue;
        if (tokenToProfile(val, profile)) applied = true;
    }
    f.close();
    return applied;
}

bool writeLastProfileToSD(MissionProfile profile) {
    // Runtime call from ui_actions.cpp — same bounded 2000ms wait
    // writeDisplaySettingsToSD() uses, not portMAX_DELAY: a menu-driven
    // profile switch isn't worth stalling indefinitely for.
    SpiBusLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) return false;

    return writeProfileStateFile(profile);
}
