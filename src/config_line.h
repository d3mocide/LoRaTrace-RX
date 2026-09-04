#pragma once
// LoRaTrace RX — shared parsing for the `key=value` settings files on SD.
//
// Every settings module (display_settings.h, region_settings.h,
// sweep_margin_settings.h, capture_settings.h) had its own byte-identical
// copy of "trim the line, skip blanks and #comments, split on the first =,
// trim both halves, reject empties" — about fourteen lines each, four
// times, with the per-key validation as the only real difference. This is
// that shared half (docs/research/2026-09-04-project-audit.md, L1).
//
// Pure by design: no Arduino, no String, no SD. That's what lets the
// modules' own apply...() functions live in their headers and be
// host-tested (`pio test -e native`, test/test_config_line/ and
// test/test_settings_parse/) instead of only being exercised by booting
// real hardware with a real card — same reasoning as gps_parse.h,
// keyboard.h and ui_menu.h. The audit's M5 was that none of this
// validation had a single test behind it.
//
// Callers keep reading lines however they like (Arduino `String` from
// File::readStringUntil() is fine) and pass `.c_str()` in.

#include <stddef.h>
#include <string.h>

inline bool configLineIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Copies [begin, end) with surrounding whitespace removed. Fails on an
// empty result or anything that would not fit `outSize` including its
// terminator — a truncated key silently matching some other key would be
// worse than rejecting the line.
inline bool configLineCopyTrimmed(const char *begin, const char *end, char *out, size_t outSize) {
    if (begin == nullptr || end == nullptr || out == nullptr || outSize == 0) return false;
    while (begin < end && configLineIsSpace(*begin)) begin++;
    while (end > begin && configLineIsSpace(end[-1])) end--;
    const size_t len = (size_t)(end - begin);
    if (len == 0 || len + 1 > outSize) return false;
    memcpy(out, begin, len);
    out[len] = '\0';
    return true;
}

// Splits one config line into trimmed key/value. Returns false — meaning
// "nothing applied", never "file is corrupt" — for a blank line, a
// #comment, a missing '=', an empty key or value, or a half that doesn't
// fit the caller's buffer. Splits on the FIRST '=' so a value may contain
// one.
inline bool configLineSplit(const char *raw, char *key, size_t keySize, char *value,
                            size_t valueSize) {
    if (raw == nullptr) return false;
    const char *p = raw;
    while (*p != '\0' && configLineIsSpace(*p)) p++;
    if (*p == '\0' || *p == '#') return false;

    const char *eq = strchr(p, '=');
    if (eq == nullptr) return false;

    if (!configLineCopyTrimmed(p, eq, key, keySize)) return false;
    const char *valueBegin = eq + 1;
    return configLineCopyTrimmed(valueBegin, valueBegin + strlen(valueBegin), value, valueSize);
}

// Strict base-10 parse: the whole token must be an optionally-signed run of
// digits.
//
// This exists because Arduino's String::toInt() returns 0 for input it
// cannot parse, with no way to tell that apart from a real "0". That is
// harmless where 0 is out of range (a garbage margin_dbm_x10 fails its
// range check anyway) but silently wrong where 0 is *valid*: before this,
// `window_index=garbage` parsed as 0 and quietly selected Capture: OFF, and
// `idle_timeout_index=garbage` quietly selected Idle dim: Off. A corrupt
// line should be ignored so the struct default survives, not be honoured as
// a real setting.
//
// Bounded to signed 32-bit rather than LONG_MAX so behaviour is identical
// on the 32-bit target and the 64-bit host that tests it.
inline bool configParseLong(const char *text, long &out) {
    if (text == nullptr || *text == '\0') return false;
    const char *p = text;
    bool negative = false;
    if (*p == '-' || *p == '+') {
        negative = (*p == '-');
        p++;
    }
    if (*p == '\0') return false;

    long value = 0;
    for (; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        const long digit = (long)(*p - '0');
        if (value > (2147483647L - digit) / 10L) return false; // overflow
        value = value * 10L + digit;
    }
    out = negative ? -value : value;
    return true;
}

inline bool configParseLongInRange(const char *text, long lo, long hi, long &out) {
    long value = 0;
    if (!configParseLong(text, value)) return false;
    if (value < lo || value > hi) return false;
    out = value;
    return true;
}
