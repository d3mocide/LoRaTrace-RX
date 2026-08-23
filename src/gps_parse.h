#pragma once
// LoRaTrace RX — GPS fix state, and the pure function that advances it.
//
// The parsing half of gps_task, split out so it's testable on the host
// (`pio test -e native`). gps_task.cpp is then a thin wrapper: read bytes
// from UART1, hand complete sentences to gpsApplySentence(), publish the
// result under a mutex. No FreeRTOS or Arduino types appear here.

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "nmea.h"

// Satellites in view for one constellation. Tracked per talker ID because
// each constellation emits its own GSV: GP=GPS, GL=GLONASS, GA=Galileo,
// BD/GB=BeiDou, GQ=QZSS.
struct GpsTalkerSats {
    char id[3];
    uint8_t in_view;
};
constexpr size_t GPS_MAX_TALKERS = 6;

// A GPS fix as the logger and UI need it. Not queued (the Detection struct
// stays small and GPS-free — see detection.h), so size is not
// performance-critical here.
struct GpsFix {
    double lat = 0.0;
    double lon = 0.0;

    // millis() at last successful update, for staleness checks. 0 = never.
    uint32_t updated_ms = 0;

    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    uint8_t hour = 0, minute = 0, second = 0;

    uint8_t fix_quality = 0; // GGA field 6: 0 = no fix, 1 = GPS, 2 = DGPS...
    uint8_t satellites = 0;  // GGA field 7: satellites USED in the solution

    // Satellites IN VIEW, from GSV. This is the leading indicator and the
    // one that matters before a fix exists: `satellites` (used) stays 0
    // until a fix lands, so it tells you nothing about whether a cold start
    // is progressing. Zero in view across every constellation means
    // sky/antenna; some in view without a fix just means "wait longer".
    // Learned the hard way on 2026-08-23 — see PROGRESS.md.
    GpsTalkerSats talkers[GPS_MAX_TALKERS] = {};
    uint8_t talker_count = 0;
    uint8_t sats_in_view = 0; // sum across constellations

    uint8_t fix_type = 1; // GSA field 2: 1 = none, 2 = 2D, 3 = 3D

    bool has_position = false; // lat/lon are meaningful
    bool has_time = false;     // date+time are meaningful
};

// Records sats-in-view for the constellation identified by `tag`
// ("$GPGSV" -> "GP"), replacing any previous count for it.
inline void gpsNoteTalkerSats(GpsFix &fix, const char *tag, uint8_t inView) {
    if (tag == nullptr || strlen(tag) < 3) return;
    const char a = tag[1], b = tag[2]; // skip '$'

    bool found = false;
    for (uint8_t i = 0; i < fix.talker_count && !found; i++) {
        if (fix.talkers[i].id[0] == a && fix.talkers[i].id[1] == b) {
            fix.talkers[i].in_view = inView;
            found = true;
        }
    }
    if (!found && fix.talker_count < GPS_MAX_TALKERS) {
        fix.talkers[fix.talker_count].id[0] = a;
        fix.talkers[fix.talker_count].id[1] = b;
        fix.talkers[fix.talker_count].id[2] = '\0';
        fix.talkers[fix.talker_count].in_view = inView;
        fix.talker_count++;
    }

    uint16_t total = 0;
    for (uint8_t i = 0; i < fix.talker_count; i++) {
        total = (uint16_t)(total + fix.talkers[i].in_view);
    }
    fix.sats_in_view = (uint8_t)(total > 255 ? 255 : total);
}

// Parses "HHMMSS" (with optional fractional seconds, which we discard —
// sub-second precision is meaningless next to LoRa airtime and queue
// latency). Returns false unless all three components are present and in
// range.
inline bool gpsParseTimeField(const char *t, uint8_t *h, uint8_t *m, uint8_t *s) {
    if (t == nullptr || strlen(t) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (t[i] < '0' || t[i] > '9') return false;
    }
    uint8_t hh = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
    uint8_t mm = (uint8_t)((t[2] - '0') * 10 + (t[3] - '0'));
    uint8_t ss = (uint8_t)((t[4] - '0') * 10 + (t[5] - '0'));
    if (hh > 23 || mm > 59 || ss > 60) return false; // 60 allows a leap second
    *h = hh; *m = mm; *s = ss;
    return true;
}

// Parses RMC's "DDMMYY" date field. Two-digit years are windowed to
// 2000-2099: NMEA offers nothing better, and this firmware did not exist
// before 2025.
inline bool gpsParseDateField(const char *d, uint16_t *year, uint8_t *month, uint8_t *day) {
    if (d == nullptr || strlen(d) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (d[i] < '0' || d[i] > '9') return false;
    }
    uint8_t dd = (uint8_t)((d[0] - '0') * 10 + (d[1] - '0'));
    uint8_t mo = (uint8_t)((d[2] - '0') * 10 + (d[3] - '0'));
    uint16_t yy = (uint16_t)((d[4] - '0') * 10 + (d[5] - '0'));
    if (dd < 1 || dd > 31 || mo < 1 || mo > 12) return false;
    *day = dd; *month = mo; *year = (uint16_t)(2000 + yy);
    return true;
}

// Applies one NMEA sentence to `fix`. `now_ms` is the caller's millis().
// Returns true if anything was updated.
//
// Only GGA and RMC are consumed; other sentences are ignored rather than
// treated as errors (a module emits plenty of GSV/GSA/VTG traffic that is
// perfectly valid and simply not useful here).
//
// Sentences failing checksum are rejected outright. A GPS module's UART is
// exactly the kind of line that picks up corruption, and a plausible-looking
// but corrupt position written to SD is worse than no position at all.
inline bool gpsApplySentence(GpsFix &fix, const char *sentence, uint32_t now_ms) {
    if (sentence == nullptr || sentence[0] != '$') return false;
    if (!nmeaChecksumValid(sentence)) return false;

    char tag[8];
    if (!nmeaField(sentence, 0, tag, sizeof(tag))) return false;

    // Accept any talker ID (GP=GPS, GN=multi-GNSS, GL=GLONASS, BD/GB=BeiDou).
    // The ATGM336H on this board emits GN* when it has multiple
    // constellations, so matching only "GP" would silently ignore it.
    const bool isGGA = (strstr(tag, "GGA") != nullptr);
    const bool isRMC = (strstr(tag, "RMC") != nullptr);
    const bool isGSV = (strstr(tag, "GSV") != nullptr);
    const bool isGSA = (strstr(tag, "GSA") != nullptr);
    if (!isGGA && !isRMC && !isGSV && !isGSA) return false;

    char buf[16], hemi[4];
    bool updated = false;

    if (isGSV) {
        // GSV field 3 = satellites in view for this constellation.
        if (nmeaField(sentence, 3, buf, sizeof(buf)) && buf[0] != '\0') {
            long n = strtol(buf, nullptr, 10);
            if (n >= 0 && n <= 255) {
                gpsNoteTalkerSats(fix, tag, (uint8_t)n);
                updated = true;
            }
        }
        return updated;
    }

    if (isGSA) {
        // GSA field 2 = fix type: 1 none, 2 = 2D, 3 = 3D. Multi-constellation
        // receivers emit several GSA sentences per cycle (one per
        // constellation), so keep the best rather than the last — otherwise
        // a trailing "no fix" GSA for an unused constellation would clobber
        // a real 3D fix reported by another. GGA below resets it back to 1
        // when the receiver reports no fix, so "best" cannot latch high
        // forever once the fix is genuinely lost.
        if (nmeaField(sentence, 2, buf, sizeof(buf)) && buf[0] != '\0') {
            long t = strtol(buf, nullptr, 10);
            if (t >= 1 && t <= 3 && (uint8_t)t > fix.fix_type) {
                fix.fix_type = (uint8_t)t;
                updated = true;
            }
        }
        return updated;
    }

    if (isGGA) {
        // GGA: 1=time 2=lat 3=N/S 4=lon 5=E/W 6=quality 7=satellites
        uint8_t h, m, s;
        if (nmeaField(sentence, 1, buf, sizeof(buf)) && gpsParseTimeField(buf, &h, &m, &s)) {
            fix.hour = h; fix.minute = m; fix.second = s;
            updated = true;
        }
        if (nmeaField(sentence, 6, buf, sizeof(buf)) && buf[0] != '\0') {
            long q = strtol(buf, nullptr, 10);
            fix.fix_quality = (q < 0) ? 0 : (uint8_t)(q > 255 ? 255 : q);
            // GGA is authoritative on whether a fix exists at all, and it
            // arrives once per cycle — so it's the right place to decay the
            // GSA-derived 2D/3D value. Without this, fix_type would latch at
            // its best-ever reading and keep claiming 3D after signal loss.
            if (fix.fix_quality == 0) fix.fix_type = 1;
            updated = true;
        }
        if (nmeaField(sentence, 7, buf, sizeof(buf)) && buf[0] != '\0') {
            long n = strtol(buf, nullptr, 10);
            fix.satellites = (n < 0) ? 0 : (uint8_t)(n > 255 ? 255 : n);
            updated = true;
        }

        // Position only counts when the module claims an actual fix. A
        // no-fix GGA carries empty lat/lon fields, and nmeaCoordToDegrees
        // rejects those — but checking quality first makes the intent
        // explicit and guards against a module that emits stale values.
        double lat, lon;
        if (fix.fix_quality > 0 &&
            nmeaField(sentence, 2, buf, sizeof(buf)) &&
            nmeaField(sentence, 3, hemi, sizeof(hemi)) &&
            nmeaCoordToDegrees(buf, hemi[0], &lat) &&
            nmeaField(sentence, 4, buf, sizeof(buf)) &&
            nmeaField(sentence, 5, hemi, sizeof(hemi)) &&
            nmeaCoordToDegrees(buf, hemi[0], &lon)) {
            fix.lat = lat;
            fix.lon = lon;
            fix.has_position = true;
            fix.updated_ms = now_ms;
            updated = true;
        }
    }

    if (isRMC) {
        // RMC: 1=time 2=status(A/V) 3=lat 4=N/S 5=lon 6=E/W 9=date
        char status[4] = {0};
        nmeaField(sentence, 2, status, sizeof(status));
        const bool active = (status[0] == 'A');

        uint8_t h, m, s;
        const bool haveTime =
            nmeaField(sentence, 1, buf, sizeof(buf)) && gpsParseTimeField(buf, &h, &m, &s);
        if (haveTime) {
            fix.hour = h; fix.minute = m; fix.second = s;
            updated = true;
        }

        uint16_t y; uint8_t mo, d;
        if (nmeaField(sentence, 9, buf, sizeof(buf)) && gpsParseDateField(buf, &y, &mo, &d)) {
            fix.year = y; fix.month = mo; fix.day = d;
            // Date and time together are what make a usable UTC stamp;
            // RMC is the only sentence here carrying the date.
            if (haveTime) fix.has_time = true;
            updated = true;
        }

        double lat, lon;
        if (active &&
            nmeaField(sentence, 3, buf, sizeof(buf)) &&
            nmeaField(sentence, 4, hemi, sizeof(hemi)) &&
            nmeaCoordToDegrees(buf, hemi[0], &lat) &&
            nmeaField(sentence, 5, buf, sizeof(buf)) &&
            nmeaField(sentence, 6, hemi, sizeof(hemi)) &&
            nmeaCoordToDegrees(buf, hemi[0], &lon)) {
            fix.lat = lat;
            fix.lon = lon;
            fix.has_position = true;
            fix.updated_ms = now_ms;
            updated = true;
        }
        if (!active) {
            // Explicitly drop the position claim when the receiver says the
            // fix went void; keeping the last good lat/lon would quietly
            // attribute new detections to an old location.
            fix.has_position = false;
        }
    }

    return updated;
}

// True when the fix is recent enough to attribute a detection to. Age is
// deliberately the caller's policy, not a constant baked in here.
inline bool gpsFixIsFresh(const GpsFix &fix, uint32_t now_ms, uint32_t max_age_ms) {
    if (!fix.has_position || fix.updated_ms == 0) return false;
    return (uint32_t)(now_ms - fix.updated_ms) <= max_age_ms;
}
