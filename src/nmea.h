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

// Longest legal NMEA sentence is 82 chars including delimiters; round up.
constexpr size_t NMEA_MAX_SENTENCE = 96;

// Pulls field `index` (0 = the "$GPGGA" talker/tag itself) out of an NMEA
// sentence into `out`. Returns false if the field doesn't exist. Stops at a
// '*' (checksum delimiter) or NUL as well as ','. Always NUL-terminates on
// success; never writes past `outSize`.
inline bool nmeaField(const char *s, uint8_t index, char *out, size_t outSize) {
    if (out == nullptr || outSize == 0) return false;
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
        if (field == index && w + 1 < outSize) out[w++] = *p;
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
    if (p[1] == '\0' || p[2] == '\0') return false;

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

    const char *dot = strchr(value, '.');
    if (dot == nullptr) return false;
    // Degrees are everything before the final two digits of the whole part.
    size_t wholeLen = (size_t)(dot - value);
    if (wholeLen < 3) return false; // need at least d + mm
    size_t degLen = wholeLen - 2;

    char degBuf[8];
    if (degLen + 1 > sizeof(degBuf)) return false;
    memcpy(degBuf, value, degLen);
    degBuf[degLen] = '\0';

    char *end = nullptr;
    double degrees = strtod(degBuf, &end);
    if (end == degBuf || *end != '\0') return false;

    end = nullptr;
    double minutes = strtod(value + degLen, &end);
    if (end == value + degLen || *end != '\0') return false;
    if (minutes < 0.0 || minutes >= 60.0) return false;

    double result = degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') result = -result;
    else if (hemi != 'N' && hemi != 'E') return false;

    *out = result;
    return true;
}
