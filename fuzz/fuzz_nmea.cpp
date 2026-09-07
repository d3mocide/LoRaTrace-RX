// Audit A27: NMEA sentence parsing, fed arbitrary bytes. A GPS module is not
// an attacker, but a damaged antenna feed, a partially received line, or a
// spoofed module all produce exactly this input, and a wrong position is worse
// than a missing one.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/gps_parse.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 512) return 0;

    // NUL-terminated copy: gpsApplySentence takes a C string.
    char sentence[513];
    memcpy(sentence, data, size);
    sentence[size] = '\0';

    GpsFix fix;
    static uint32_t clock_ms = 1000;
    clock_ms += 137;
    gpsApplySentence(fix, sentence, clock_ms);

    // A fix that claims a position must claim one that exists. This is the
    // property, not the parse: nmeaCoordToDegrees used to accept 99 degrees
    // of latitude (audit A06).
    if (fix.has_position) {
        if (!(fix.lat >= -90.0 && fix.lat <= 90.0)) __builtin_trap();
        if (!(fix.lon >= -180.0 && fix.lon <= 180.0)) __builtin_trap();
    }
    if (fix.has_time) {
        if (fix.month < 1 || fix.month > 12) __builtin_trap();
        if (fix.day < 1 || fix.day > 31) __builtin_trap();
        if (fix.hour > 23 || fix.minute > 59 || fix.second > 60) __builtin_trap();
    }
    // Field extraction must never report success with an unterminated buffer.
    for (uint8_t f = 0; f < 20; f++) {
        char field[8];
        memset(field, 0x5A, sizeof(field));
        if (nmeaField(sentence, f, field, sizeof(field))) {
            if (memchr(field, '\0', sizeof(field)) == nullptr) __builtin_trap();
        }
    }
    return 0;
}
