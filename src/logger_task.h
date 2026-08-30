#pragma once
// LoRaTrace RX — logger task (Core 0).
//
// Dequeues Detections, stamps each with the newest GPS fix, and writes them
// to SD in batches per docs/DESIGN.md §2/§8. Probe observations are written to a
// separate durable file so CAD-only activity never masquerades as a packet.
//
// Batching is the core design decision here, and it's about bus time, not
// throughput. SD and the SX1262 share one physical bus (spi_bus.h), and an
// SD card can stall for 100ms+ on internal housekeeping mid-write. Every
// millisecond the logger holds the bus is a millisecond the radio can't
// read a packet out of the SX1262's FIFO. So:
//
//   * Accumulate rows in a small RAM buffer and write them in one go —
//     far fewer bus acquisitions than per-row writes.
//   * Keep each flush SHORT rather than rare. The FIFO gives roughly one
//     packet-time of slack, so many small flushes beat occasional huge
//     ones. This is why the buffer is ~2KB and not 32KB.
//   * Open, append, close per flush. Holding the file open across a
//     wardriving session risks a truncated log the moment the device is
//     unplugged, which is how this device will always be turned off.
//
// Live traffic makes this sharper than it looks: Meshtastic rebroadcasts
// mean packets frequently arrive in pairs milliseconds apart
// (docs/history/CHANGELOG.md, 2026-08-23), so back-to-back RX is the common case, not the rare one.
//
// This task writes four files, all inside THIS RUN's own directory —
// /loratrace/runNNNN/ (run_log.h). One wardrive is one folder, so a drive
// can be copied, shared or deleted as a unit instead of being carved out of
// one ever-growing file:
//   detections.csv — the mission data, docs/DESIGN.md §8 schema
//   session.csv    — one health row a minute (session_log.h), so an
//                    unattended run leaves evidence of whether it held up
//                    instead of only evidence of what it heard
//   probe.csv      — one bounded CAD observation per Probe candidate
//   nodes.csv      — decoded MeshTastic node identity observations

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "energy_observation.h"
#include "node_identity.h"
#include "scan_observation.h"

// Starts the task on Core 0. `queue` supplies Detections from the radio
// task, `scanQueue` supplies fixed CAD observations from Probe, and
// `energyQueue` supplies fixed energy-peak observations from Sweep.
// `initialSdMounted` is the boot-time config reader's mount result: when it
// is true, the logger adopts that already-working mount rather than tearing
// it down and immediately remounting it. Returns false if the task could
// not be created.
bool loggerTaskStart(QueueHandle_t queue, QueueHandle_t scanQueue, QueueHandle_t energyQueue,
                     QueueHandle_t identityQueue, bool initialSdMounted);

// True once SD is mounted and the log file is writable. When false the
// task keeps draining the queue and discarding — a missing card must not
// back-pressure the receiver.
bool loggerSdReady();

// Queues one explicit SD remount attempt for an offline logger (for example
// after the operator reseats a card). There is deliberately no automatic
// retry loop: an electrically sick card must leave the receiver usable,
// rather than repeatedly consuming the logger task in synchronous SD I/O.
// Returns false when SD is already ready or the logger has not started.
bool loggerRequestSdRetry();

// --- Diagnostics -------------------------------------------------------
uint32_t loggerRowsWritten();
uint32_t loggerRowsDropped();  // dequeued but unwritable (no SD, format fail)
uint32_t loggerFlushCount();
uint32_t loggerMaxFlushMs();   // worst DETECTION-flush bus hold; the number
                               // that decides whether batch sizing is
                               // hurting the radio
uint32_t loggerMaxSessionMs(); // worst HEALTH-row bus hold. Tracked apart
                               // from the flush metric so a once-a-minute
                               // instrumentation write can never be mistaken
                               // for evidence that batching needs retuning.
                               // Worst hold overall = max of the two.
uint32_t loggerSessionRows();  // health rows committed to this run's session.csv
uint32_t loggerScanRowsWritten();
uint32_t loggerScanRowsDropped();
uint32_t loggerEnergyRowsWritten();
uint32_t loggerEnergyRowsDropped();
uint32_t loggerIdentityRowsWritten();
uint32_t loggerIdentityRowsDropped();

// This power-on's run index, or 0 before SD has mounted. Matches the
// runNNNN directory the logs are being written into.
uint16_t loggerRunIndex();

// --- Debug mode ----------------------------------------------------------
// Menu-toggled (ui_task.cpp), off by default. When on, every detection
// prints a one-line summary (profile/RSSI/SNR/SF/BW/timing) to Serial the
// moment it's dequeued — added so RSSI/SNR (normally only ever written to
// detections.csv) can be sanity-checked live without pulling the SD card or
// standing up the WiFi AP. Lives here rather than in radio_task.cpp on
// purpose: printing happens on Core 0 after the queue hop, so a slow/absent
// serial console can never back-pressure the Core 1 radio task the way a
// print from inside its own loop could (CLAUDE.md: radio task must never
// block on non-radio I/O).
void loggerDebugToggle();
// Idempotent counterpart for the authenticated local web panel.  Keeps the
// menu toggle and web setting on the same Core 0-owned logger state and the
// same serial header/notice behavior.
void loggerDebugSetEnabled(bool enabled);
bool loggerDebugIsEnabled();
