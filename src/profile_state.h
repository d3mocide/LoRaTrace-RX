#pragma once
// LoRaTrace RX — last-active profile persistence, separate from config.h's
// channel-override scope (that file documents itself as narrowly about
// freq/SF/BW/CR, not which profile boots).
//
// Without this, main.cpp always boots MissionProfile::MESHTASTIC regardless
// of what was active at the last power-off, even though channel overrides
// for the *other* profile were already being remembered — a MeshCore
// operator's radio silently came back up listening for Meshtastic on every
// reboot. Mirrors display_settings.h/.cpp's load/write pair exactly:
//   - loadLastProfileFromSD() is boot-time, called from main.cpp's setup()
//     after loadProfileOverridesFromSD() has already mounted the card.
//   - writeLastProfileToSD() is the runtime entry point (ui_actions.cpp,
//     right after a menu-driven profile switch), and arbitrates spi_bus.h's
//     mutex itself the same way writeDisplaySettingsToSD() does.
//
// File: /loratrace/profile.txt, sibling to config.txt/display.txt. Fails
// safe: missing card/file/bad value leaves the caller's profile untouched.

#include "channel_plans.h"

bool loadLastProfileFromSD(MissionProfile &profile);
bool writeLastProfileToSD(MissionProfile profile);
