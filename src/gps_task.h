#pragma once
// LoRaTrace RX — GPS task (Core 0).
//
// Reads NMEA from UART1 and publishes the newest fix behind a mutex, per
// docs/DESIGN.md §2 ("GPS Task -> last-fix (mutex)"). All the actual parsing
// lives in gps_parse.h as pure functions so it can be tested on the host;
// this file is only the FreeRTOS/UART wrapper around it.
//
// The GPS shares no bus with the radio (it's a plain UART, not the SPI the
// SX1262 and SD card fight over), so it needs no spi_bus arbitration. It
// does need its own mutex because the logger reads the fix from another
// task, and a partially-updated double is a garbage coordinate.

#include <freertos/FreeRTOS.h>

#include "gps_parse.h"

// Starts UART1 and the task pinned to Core 0. Returns false if the task or
// its mutex could not be created. Safe to call once from setup().
bool gpsTaskStart();

// Copies the current fix. Returns false if the mutex could not be taken in
// time (callers should treat that as "no fix available right now" rather
// than blocking — the logger must keep draining the queue regardless).
bool gpsGetFix(GpsFix &out, TickType_t timeout);

// Diagnostics for the periodic status line.
uint32_t gpsSentenceCount();
uint32_t gpsChecksumErrorCount();

// Worst-case gap (ms) between two passes of gpsTask()'s drain loop, i.e. the
// longest this task ever went without a chance to empty the UART ring
// buffer. Direct evidence for or against the "SD/logger activity starves the
// GPS task and the ring buffer overflows" hypothesis (docs/history/PROGRESS.md,
// nmea_bad_crc watch item) — the theory predicts this spikes during SD
// writes; a checksum-error count alone can't say whether that's the actual
// mechanism or just correlated with something else.
uint32_t gpsMaxLoopGapMs();

// Count of lines discarded for overrunning the 96-byte assembly buffer
// before a terminator arrived. Previously silent (the code just resynced
// and moved on) — but a dropped byte that happens to be a sentence's own
// '\n'/'\r' produces exactly this symptom, and it carries none of the
// bytes lost to it into nmea_bad_crc, so the true corruption rate could be
// higher than that counter alone reports.
uint32_t gpsOversizeDropCount();

// millis() at the first position fix since power-on, or 0 if there has not
// been one yet. Time-to-first-fix is an operational number for a wardriver
// — it says how long after switching on the track becomes usable — and it
// can only be measured across a whole session, so it is recorded as the
// session runs rather than reconstructed afterwards.
uint32_t gpsFirstFixMillis();

// True once GPS UTC has been adopted as the device's system clock. Until
// then every file written to SD carries FAT's 1980 epoch rather than a real
// date, so this is what says whether the card's own timestamps mean
// anything. Set from time alone, not position — GPS has the time long
// before it has a fix.
bool gpsSystemTimeSet();
