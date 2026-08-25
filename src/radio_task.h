#pragma once
// LoRaTrace RX — radio task (Core 1, highest priority).
//
// Owns the SX1262 exclusively and implements HOME_LISTEN from DESIGN.md §5:
// continuous RX locked to the active profile's channel; on a valid packet,
// push a Detection into the queue and stay locked. Phase 4 adds the other
// half of §5's state machine text — "operator-selected... mutually
// exclusive" — as a live retune via radioRequestProfileSwitch(), below.
// DISCOVERY_SWEEP and ENERGY_SWEEP are phases 7/8 and are still deliberately
// absent — the state machine's shape is here, those two states are not.
//
// The one hard rule (DESIGN.md §2, CLAUDE.md): this task never touches SD
// or the display, and never blocks on another task. It reads the packet,
// re-arms RX, drops a ~36-byte struct in a queue, and goes back to
// listening. If the queue is full it *drops the detection and counts it*
// rather than waiting — a receiver that stalls to preserve a log entry is
// strictly worse than one that misses the entry.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "channel_plans.h"
#include "detection.h"

// Starts the SX1262 on `channel`/`profile` and launches the task on Core 1.
// `queue` receives Detection structs; it must outlive the task. `overrides`
// is the per-profile SD/web config main.cpp already loaded (config.h) —
// copied in and held for the task's lifetime so a later
// radioRequestProfileSwitch() resolves each profile's *current* override
// rather than always falling back to channel_plans.h's hardcoded table
// (the pre-2026-08-24 bug: switching away from a profile and back silently
// dropped its override — see PROGRESS.md Decisions log).
// Returns false if the radio failed to initialise or the task couldn't be
// created — callers should treat that as fatal, since a wardriver with no
// receiver has nothing to do.
bool radioTaskStart(const ChannelParams &channel, MissionProfile profile,
                    const ProfileOverrides &overrides, QueueHandle_t queue);

// Last RadioLib error code from begin()/startReceive() — including a live
// profile switch's own begin() call, so a failed switch is visible the same
// way a failed boot is.
int radioLastError();

// The mission profile HOME_LISTEN is actually locked to right now. Updated
// the instant a requested switch (below) actually lands on the radio, not
// the instant it's requested — same "small POD, no lock" caveat as
// radioActiveChannel() below.
MissionProfile radioActiveProfile();

// DESIGN.md §5's operator-selected, mutually-exclusive profile switch:
// Meshtastic and MeshCore never listen at once. Queues a retune to
// `profile`'s channel table (channel_plans.h) and wakes the radio task to
// pick it up between packets — never blocks, so it's safe to call from
// ui_task's keyboard poll. A packet mid-flight on the old channel at the
// instant of the switch is lost; that's the accepted cost of "mutually
// exclusive," not a bug. Returns false if the radio task hasn't started yet.
bool radioRequestProfileSwitch(MissionProfile profile);

// --- Diagnostics -------------------------------------------------------
// Exposed because Phase 2's exit criterion is "no dropped packets
// attributable to SD latency" — that claim is only checkable if drops are
// counted rather than silently absorbed.
uint32_t radioPacketCount();     // successfully decoded
uint32_t radioCrcErrorCount();   // received but failed CRC
uint32_t radioQueueDropCount();  // decoded but the queue was full
uint32_t radioBusMissCount();    // couldn't get the SPI bus in time

// The channel table HOME_LISTEN is locked to right now (post SD-config-
// override at boot; post radioRequestProfileSwitch() if one has landed
// since — that's still the only hot-reload path, config.txt itself is still
// boot-time-only, see config.h). A read-only copy, safe to call from any
// task: wifi_task's settings page uses this to show the current values, not
// just the last thing config.txt said, since a bad/missing SD card at boot
// or a runtime profile switch both mean those can differ.
ChannelParams radioActiveChannel();

// The per-profile SD/web overrides loaded at boot (config.h), exactly as
// radioTaskStart() received them — this task's copy is the one
// radioRequestProfileSwitch() actually resolves against, so wifi_task's
// settings page reads from here rather than re-parsing config.txt itself
// (which would risk disagreeing with what the radio is actually doing).
// Same small-POD, no-lock convention as radioActiveChannel() above. Does
// NOT reflect a save made through writeProfileConfigToSD() until the next
// boot — same "not live" boundary as the channel override itself.
ProfileOverrides radioActiveOverrides();
