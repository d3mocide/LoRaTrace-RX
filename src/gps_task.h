#pragma once
// LoRaTrace RX — GPS task (Core 0).
//
// Reads NMEA from UART1 and publishes the newest fix behind a mutex, per
// DESIGN.md §2 ("GPS Task -> last-fix (mutex)"). All the actual parsing
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
