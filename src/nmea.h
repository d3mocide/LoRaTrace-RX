#pragma once
// LoRaTrace RX — NMEA 0183 parsing primitives.
//
// Deliberately dependency-free and hardware-free: no Arduino types, no
// dynamic allocation, no TinyGPS++. Two reasons, both learned the hard way
// on this project:
//
//   1. Everything here is pure logic, so it runs under `pio test -e native`
//      on the host. This board is slow and awkward to test on, and the
//      2026-08-23 sync-word bug showed what happens when a wrong constant
//      only reveals itself through hours of bench work. Parsing bugs should
//      die on the host, not on a hillside.
//   2. `gps_probe.cpp` is a *bring-up* tool — if it depended on a GPS
//      library, a library problem would masquerade as a wiring problem,
//      which is exactly the confusion the probe exists to eliminate.
//
// Shared by gps_probe.cpp (the standalone probe) and gps_task.cpp (the
// Phase 2 task) so both agree on what a sentence means.

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Longest legal NMEA sentence is 82 chars including delimiters; round up.
constexpr size_t NMEA_MAX_SENTENCE = 96;

// Pulls field `index` (0 = the "$GPGGA" talker/tag itself) out of an NMEA
// sentence into `out`. Returns false if the field doesn't exist. Stops at a
// '*' (checksum delimiter) or NUL as well as ','. Always NUL-terminates on
// success; never writes past `outSize`.
inline bool nmeaField(const char *s, uint8_t index, char *out, size_t outSize) {
    if (s == nullptr || out == nullptr || outSize == 0) return false;
    uint8_t field = 0;
    size_t w = 0;
    for (const char *p = s;; p++) {
        if (*p == ',' || *p == '\0' || *p == '*') {
            if (field == index) {
                out[w] = '\0';
                return true;
            }
            field++;
            w = 0;
            if (*p == '\0' || *p == '*') return false;
            continue;
        }
        if (field == index) {
            if (w + 1 >= outSize) { out[0] = '\0'; return false; }
            out[w++] = *p;
        }
    }
}

// Validates the "*HH" trailing checksum: XOR of every byte between '$' and
// '*'. Returns false when the sentence has no checksum at all — callers get
// to decide whether that's acceptable, but for a wardriving log it isn't:
// a corrupted sentence that still parses would silently write a wrong
// position to SD, which is worse than dropping the fix.
inline bool nmeaChecksumValid(const char *s) {
    if (s == nullptr || *s != '$') return false;
    uint8_t sum = 0;
    const char *p = s + 1;
    for (; *p && *p != '*'; p++) sum ^= (uint8_t)*p;
    if (*p != '*') return false;
    if (p[1] == '\0' || p[2] == '\0' || p[3] != '\0') return false;

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int hi = hexVal(p[1]), lo = hexVal(p[2]);
    if (hi < 0 || lo < 0) return false;
    return sum == (uint8_t)((hi << 4) | lo);
}

// Converts an NMEA coordinate ("ddmm.mmmm" / "dddmm.mmmm") plus its
// hemisphere character into signed decimal degrees. Returns false on an
// empty or malformed value — importantly including the empty fields a
// module emits before it has a fix, which must NOT be read as 0.0 (that's
// a real location: Null Island, off the coast of Ghana).
inline bool nmeaCoordToDegrees(const char *value, char hemi, double *out) {
    if (value == nullptr || out == nullptr || value[0] == '\0') return false;

    const bool latitude = hemi == 'N' || hemi == 'S';
    if (!latitude && hemi != 'E' && hemi != 'W') return false;
    const size_t whole = latitude ? 4 : 5;
    const size_t len = strlen(value);
    if (len < whole + 2 || value[whole] != '.') return false;
    for (size_t i = 0; i < len; ++i) {
        if (i != whole && (value[i] < '0' || value[i] > '9')) return false;
    }
    unsigned degrees = 0;
    for (size_t i = 0; i < whole - 2; ++i) degrees = degrees * 10 + value[i] - '0';
    const double minutes = strtod(value + whole - 2, nullptr);
    const unsigned limit = latitude ? 90 : 180;
    if (!isfinite(minutes) || minutes >= 60 || degrees > limit ||
        (degrees == limit && minutes != 0)) return false;
    double result = degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') result = -result;
    *out = result;
    return true;
}
