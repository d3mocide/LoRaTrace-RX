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
// Fails safe: missing SD card, missing file, or bad values fall back to
// whatever ChannelParams the caller already had (the hardcoded default in
// channel_plans.h) rather than hanging or radioing on a garbage frequency.
//
// First card seen by this firmware gets /loratrace/config.txt created
// automatically, pre-filled with the current defaults — an operator edits
// that file in place rather than hand-copying sd-template/loratrace/
// themselves. sd-template/ stays around as an offline reference/example.

#include <SPI.h>

#include "channel_plans.h"

// SD path, matching BRAND.md's /loratrace/ namespace convention. See
// sd-template/loratrace/config.txt for the format, comments, and a
// working example.
constexpr const char *CHANNEL_CONFIG_DIR = "/loratrace";
constexpr const char *CHANNEL_CONFIG_PATH = "/loratrace/config.txt";

// Mounts SD on `spi`/`csPin`. Creates /loratrace/config.txt pre-filled with
// `params` if it doesn't exist yet (first card seen by this firmware), then
// — once the file exists, whether just-created or from a prior boot — opens
// it and applies any recognized keys on top of `params` (in place).
// Out-of-range values
// (outside the module's 868-928MHz tuned range, SF 5-12, CR 5-8 — DESIGN.md
// §1/§3) are rejected field-by-field with a warning rather than applied.
// `sync_word` accepts hex ("0x2B") or decimal and any value 0x00-0xFF —
// deliberately unrestricted, since sniffing an unknown protocol is a
// legitimate reason to try an arbitrary one.
// Returns true if at least one field was overridden from the file, false
// if `params` is unchanged (no card, no file, or nothing valid in it) —
// either way, `params` is left in a valid state safe to pass to
// radio.begin().
bool loadChannelConfigFromSD(ChannelParams &params, int8_t csPin, SPIClass &spi);

// The same bounds `loadChannelConfigFromSD` applies field-by-field, exposed
// so a caller (wifi_task's settings endpoint) can validate before writing
// rather than duplicating these numbers a second time.
bool channelFreqInRange(float mhz);
bool channelSfInRange(uint8_t sf);
bool channelCrInRange(uint8_t cr);

// Runtime settings-save path (wifi_task's web UI): validates every field of
// `params` against the same bounds `loadChannelConfigFromSD` enforces and,
// only if all pass, overwrites CHANNEL_CONFIG_PATH with them — applied on
// the *next* boot, not live (radio_task's running SX1262 is never touched
// here; see PROGRESS.md for why hot-reload was deliberately deferred).
// Unlike `loadChannelConfigFromSD` (boot-time, before any task exists),
// this runs with the radio/GPS/logger tasks already active, so it acquires
// spi_bus.h's mutex itself around the SD write — the caller does not need
// to. Returns false, without touching the card, if any field is
// out-of-range or the write itself fails (SD busy/missing/read-only).
bool writeChannelConfigToSD(const ChannelParams &params);
