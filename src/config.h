#pragma once
// LoRaTrace RX — boot-time SD config loading.
//
// Scope, deliberately narrow: override the active LoRa channel
// (freq/SF/BW/CR) from a plain-text file on SD, for operators whose local
// mesh doesn't run vanilla defaults (e.g. regional presets like
// MeshOregon). One-shot read in setup(), before the radio starts — not
// the general Logger/settings architecture Phase 2 will eventually own,
// just the one setting that blocks people from testing tonight.
//
// Fails safe: missing SD card, missing file, or bad values leave that
// profile's ProfileOverrides slot unset (channel_plans.h), so
// resolvedChannelForProfile() falls back to the hardcoded default in
// channel_plans.h rather than hanging or radioing on a garbage frequency.
//
// **Per-profile since 2026-08-24** (PROGRESS.md Decisions log): the file
// holds one override block per profile (`meshtastic_*`/`meshcore_*`
// key prefixes), not one shared block. The original single-block design
// meant a Meshtastic override and MeshCore's hardcoded table could never
// coexist, and worse, switching profiles at runtime and back would silently
// drop whatever custom override you'd set — this fixes both.
//
// First card seen by this firmware gets /loratrace/config.txt created
// automatically, pre-filled with the current defaults for both profiles —
// an operator edits that file in place rather than hand-copying
// sd-template/loratrace/ themselves. sd-template/ stays around as an
// offline reference/example.

#include <SPI.h>

#include "channel_plans.h"

// SD path, matching BRAND.md's /loratrace/ namespace convention. See
// sd-template/loratrace/config.txt for the format, comments, and a
// working example.
constexpr const char *CHANNEL_CONFIG_DIR = "/loratrace";
constexpr const char *CHANNEL_CONFIG_PATH = "/loratrace/config.txt";

// Mounts SD on `spi`/`csPin`. Creates /loratrace/config.txt pre-filled with
// both profiles' current values (`defaults` resolved against each profile's
// hardcoded table) if it doesn't exist yet (first card seen by this
// firmware), then — once the file exists, whether just-created or from a
// prior boot — opens it and applies any recognized `meshtastic_*`/
// `meshcore_*` keys into `overrides`, setting that profile's `_set` flag
// for each key it actually touched. Out-of-range values (outside the
// module's 868-928MHz tuned range, SF 5-12, CR 5-8 — DESIGN.md §1/§3) are
// rejected field-by-field with a warning rather than applied. `sync_word`
// accepts hex ("0x2B") or decimal and any value 0x00-0xFF — deliberately
// unrestricted, since sniffing an unknown protocol is a legitimate reason
// to try an arbitrary one.
// Returns true if at least one field was applied to either profile, false
// if `overrides` is unchanged (no card, no file, or nothing valid in it) —
// either way, `overrides` is left in a valid state safe to pass to
// resolvedChannelForProfile(). That return value alone can't tell a caller
// "no SD card" from "SD card fine, nothing configured on it" — both leave
// `overrides` unchanged and both return false. `sdMounted`, if non-null,
// is set to SD.begin()'s own result so a caller that needs the two apart
// (main.cpp's boot splash) doesn't have to guess from the applied-count.
bool loadProfileOverridesFromSD(ProfileOverrides &overrides, int8_t csPin, SPIClass &spi,
                                 bool *sdMounted = nullptr);

// The same bounds `loadProfileOverridesFromSD` applies field-by-field,
// exposed so a caller (wifi_task's settings endpoint) can validate before
// writing rather than duplicating these numbers a second time.
bool channelFreqInRange(float mhz);
bool channelSfInRange(uint8_t sf);
bool channelCrInRange(uint8_t cr);

// Runtime settings-save path (wifi_task's web UI): validates every field of
// `params` against the same bounds `loadProfileOverridesFromSD` enforces
// and, only if all pass, rewrites CHANNEL_CONFIG_PATH with `params` as
// `profile`'s block — `current`'s OTHER profile is written back unchanged,
// so saving Meshtastic's preset never clobbers a previously-saved MeshCore
// one (or vice versa). Applied on the *next* boot, not live (radio_task's
// running SX1262 is never touched here, and a runtime profile switch keeps
// using whatever was loaded at boot — see PROGRESS.md for why hot-reload
// was deliberately deferred). Unlike `loadProfileOverridesFromSD`
// (boot-time, before any task exists), this runs with the radio/GPS/logger
// tasks already active, so it acquires spi_bus.h's mutex itself around the
// SD write — the caller does not need to. Returns false, without touching
// the card, if any field is out-of-range or the write itself fails (SD
// busy/missing/read-only).
bool writeProfileConfigToSD(MissionProfile profile, const ChannelParams &params,
                            const ProfileOverrides &current);
