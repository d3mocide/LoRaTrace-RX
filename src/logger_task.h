#pragma once
// LoRaTrace RX — logger task (Core 0).
//
// Dequeues Detections, stamps each with the newest GPS fix, and writes them
// to SD in batches per DESIGN.md §2/§8.
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
// mean packets frequently arrive in pairs milliseconds apart (PROGRESS.md
// 2026-08-23), so back-to-back RX is the common case, not the rare one.
//
// This task writes two files, both under /loratrace:
//   detections.csv — the mission data, DESIGN.md §8 schema
//   session.csv    — one health row a minute (session_log.h), so an
//                    unattended run leaves evidence of whether it held up
//                    instead of only evidence of what it heard

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Starts the task on Core 0. `queue` supplies Detections from the radio
// task. Returns false if the task could not be created.
bool loggerTaskStart(QueueHandle_t queue);

// True once SD is mounted and the log file is writable. When false the
// task keeps draining the queue and discarding — a missing card must not
// back-pressure the receiver.
bool loggerSdReady();

// --- Diagnostics -------------------------------------------------------
uint32_t loggerRowsWritten();
uint32_t loggerRowsDropped();  // dequeued but unwritable (no SD, format fail)
uint32_t loggerFlushCount();
uint32_t loggerMaxFlushMs();   // worst-case bus hold; the number that decides
                               // whether batch sizing is hurting the radio
uint32_t loggerSessionRows();  // health rows committed to /loratrace/session.csv
