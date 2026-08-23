#pragma once
// LoRaTrace RX — the periodic session-health record.
//
// Why this file exists: Phase 2's exit criterion (ROADMAP.md) is an
// *unattended* multi-hour run — "no dropped packets attributable to SD
// latency, no crash from heap exhaustion." Every number that would settle
// that claim already exists (radio_task.h and logger_task.h expose them
// precisely because the criterion is only checkable if drops are counted),
// but until now they lived exclusively in the serial status line and the
// on-screen pages. Both require someone watching.
//
// So the one run the criterion actually names — device on battery, driven
// around for hours, unplugged at the end — was the one run that kept no
// record of itself. The CSV would show detections; nothing would show
// whether the queue ever overflowed, how long the worst bus hold was, or
// where the heap trended. "It seemed fine" is not an exit criterion.
//
// The fix is deliberately boring: the same counters, appended to SD next to
// the detections on a slow cadence. At ~1 row/minute a three-hour drive
// costs ~180 rows — nothing against a detection log, and it turns "did it
// hold up?" into a question the card answers by itself.
//
// Kept pure (no Arduino, no FreeRTOS) so `pio test -e native` covers the
// formatting, same as detection.h.

#include <stdint.h>
#include <stdio.h>

// One health sample. Plain data: logger_task.cpp fills it from the live
// counters, this header only knows how to render it.
struct SessionStats {
    // "boot" for the first row of a power-on, "periodic" thereafter. Boot
    // rows are what make session boundaries visible in a file that several
    // runs append to — without them, a power cycle mid-drive looks like the
    // counters spontaneously reset.
    const char *reason = "periodic";
    uint32_t uptime_s = 0;

    // --- GPS ---
    bool has_fix = false;
    double lat = 0.0;
    double lon = 0.0;
    uint8_t sats = 0;      // used in the solution (GGA field 7)
    uint8_t fix_type = 1;  // GSA field 2: 1 none, 2 = 2D, 3 = 3D
    // Seconds from boot to the first position fix; 0 = none yet. Recorded
    // because time-to-first-fix is an operational number for a wardriver —
    // it says how long after switching on the track is actually usable, and
    // it is only measurable across whole sessions.
    uint32_t ttff_s = 0;
    uint32_t nmea_sentences = 0;
    uint32_t nmea_bad_crc = 0;

    // --- Radio ---
    uint32_t rx = 0;
    uint32_t crc_errors = 0;
    uint32_t queue_drops = 0; // decoded but the queue was full
    uint32_t bus_misses = 0;  // radio couldn't get the SPI bus in time

    // --- Logger / SD ---
    uint32_t rows_written = 0;
    uint32_t rows_dropped = 0;
    uint32_t flushes = 0;
    uint32_t max_flush_ms = 0; // worst bus hold the logger has caused
    bool sd_ready = false;
    uint32_t bus_contention = 0;

    // --- System ---
    uint32_t heap_free = 0;
    // Low-water mark since boot. This is the field that actually answers
    // "no crash from heap exhaustion": a sample every 60s can walk straight
    // past a transient trough, and the trough is what kills the device.
    uint32_t heap_min = 0;
    uint32_t batt_mv = 0; // 0 = unknown (see battery.h — not "empty")
    // Smallest free stack the logger task has ever had, in bytes. Included
    // because this task's deepest call path now runs session-row locals on
    // top of SD/FatFS, and a stack overflow in the task that owns the card
    // is a silent, total loss of a multi-hour run. Sizing it by argument is
    // a guess; this makes the run itself answer the question.
    uint32_t logger_stack_free = 0;
};

// Column order for /loratrace/session.csv. One string so the header row and
// the row writer can't drift apart — same rule as LOG_CSV_HEADER.
constexpr const char *SESSION_CSV_HEADER =
    "timestamp_utc,uptime_s,reason,lat,lon,sats,fix_type,ttff_s,"
    "rx,crc_err,queue_drop,bus_miss,"
    "rows,row_drop,flushes,max_flush_ms,sd,bus_contention,"
    "nmea,nmea_bad_crc,heap_free,heap_min,batt_mv,logger_stack_free";

// Renders one health row into `out`. `timestamp_utc` comes from the same
// detectionFormatTimestamp() the detection rows use, and is empty before
// the GPS has a date — the two files then share a join key whenever one
// exists.
//
// Returns characters written (excluding NUL), or 0 on truncation, matching
// detectionFormatCsv()'s contract so callers treat 0 as "don't write this".
inline size_t sessionFormatCsv(const SessionStats &s, char *out, size_t outSize,
                               const char *timestamp_utc) {
    if (out == nullptr || outSize == 0) return 0;

    // Empty rather than 0,0 when there's no fix — Null Island is a real
    // coordinate and a health log that claims it is worse than one that
    // admits it doesn't know. Same convention as detectionFormatCsv().
    char latbuf[16], lonbuf[16];
    if (s.has_fix) {
        snprintf(latbuf, sizeof(latbuf), "%.6f", s.lat);
        snprintf(lonbuf, sizeof(lonbuf), "%.6f", s.lon);
    } else {
        latbuf[0] = '\0';
        lonbuf[0] = '\0';
    }

    int n = snprintf(out, outSize,
                     "%s,%lu,%s,%s,%s,%u,%u,%lu,"
                     "%lu,%lu,%lu,%lu,"
                     "%lu,%lu,%lu,%lu,%s,%lu,"
                     "%lu,%lu,%lu,%lu,%lu,%lu",
                     timestamp_utc ? timestamp_utc : "",
                     (unsigned long)s.uptime_s,
                     s.reason ? s.reason : "",
                     latbuf, lonbuf,
                     (unsigned)s.sats,
                     (unsigned)s.fix_type,
                     (unsigned long)s.ttff_s,
                     (unsigned long)s.rx,
                     (unsigned long)s.crc_errors,
                     (unsigned long)s.queue_drops,
                     (unsigned long)s.bus_misses,
                     (unsigned long)s.rows_written,
                     (unsigned long)s.rows_dropped,
                     (unsigned long)s.flushes,
                     (unsigned long)s.max_flush_ms,
                     s.sd_ready ? "ok" : "down",
                     (unsigned long)s.bus_contention,
                     (unsigned long)s.nmea_sentences,
                     (unsigned long)s.nmea_bad_crc,
                     (unsigned long)s.heap_free,
                     (unsigned long)s.heap_min,
                     (unsigned long)s.batt_mv,
                     (unsigned long)s.logger_stack_free);

    if (n < 0 || (size_t)n >= outSize) return 0; // truncated — drop the row
    return (size_t)n;
}
