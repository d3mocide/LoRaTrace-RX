#pragma once
// LoRaTrace RX — radio task (Core 1, highest priority).
//
// Owns the SX1262 exclusively and implements HOME_LISTEN from DESIGN.md §5:
// continuous RX locked to the active profile's channel; on a valid packet,
// push a Detection into the queue and stay locked. DISCOVERY_SWEEP and
// ENERGY_SWEEP are phases 4/5 and are deliberately absent — the state
// machine's shape is here, its other states are not.
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

// Starts the SX1262 on `channel` and launches the task on Core 1.
// `queue` receives Detection structs; it must outlive the task.
// Returns false if the radio failed to initialise or the task couldn't be
// created — callers should treat that as fatal, since a wardriver with no
// receiver has nothing to do.
bool radioTaskStart(const ChannelParams &channel, QueueHandle_t queue);

// Last RadioLib error code from begin()/startReceive(), for boot reporting.
int radioLastError();

// --- Diagnostics -------------------------------------------------------
// Exposed because Phase 2's exit criterion is "no dropped packets
// attributable to SD latency" — that claim is only checkable if drops are
// counted rather than silently absorbed.
uint32_t radioPacketCount();     // successfully decoded
uint32_t radioCrcErrorCount();   // received but failed CRC
uint32_t radioQueueDropCount();  // decoded but the queue was full
uint32_t radioBusMissCount();    // couldn't get the SPI bus in time

// The channel this run actually started with (post SD-config-override,
// pre any future hot-reload — there is none yet, so this is constant for
// the whole run). A read-only copy, safe to call from any task: wifi_task's
// settings page uses this to show the current values, not just the last
// thing config.txt said, since a bad/missing SD card at boot means those
// can differ.
ChannelParams radioActiveChannel();
