#pragma once
// LoRaTrace RX — spreadsheet-safe rendering of an already-written CSV row.
//
// Node names arrive over the air. `nodeIdentityCopyString()` strips control
// characters and the CSV writer quotes correctly, but a correctly quoted
// field is still interpreted as a formula by every major spreadsheet when it
// begins with =, +, - or @ (audit A12). The canonical file on the card keeps
// the literal bytes — that is the evidence — so the neutralised form is a
// separate export the operator asks for.
//
// Pure and host-tested: the transform is where the security property lives,
// and it must not be reachable only by pointing a spreadsheet at a device.

#include <stddef.h>
#include <string.h>

// The longest row any of this firmware's writers produce is
// detections.csv's raw-frame line (DETECTION_CSV_MAX_ROW, 768); a
// neutralised copy adds at most three bytes per field. One shared bound
// keeps the read and the transform buffers obviously the same size.
constexpr size_t CSV_SAFE_LINE_MAX = 1024;

// True when the whole field parses as a plain decimal number. Needed because
// half the columns here are negative dBm readings, and prefixing those would
// corrupt every RSSI value to protect against a formula they cannot be.
inline bool csvFieldIsNumber(const char *field, size_t len) {
    if (field == nullptr || len == 0) return false;
    size_t i = 0;
    if (field[i] == '-' || field[i] == '+') i++;
    bool digits = false, dot = false;
    for (; i < len; ++i) {
        if (field[i] >= '0' && field[i] <= '9') { digits = true; continue; }
        if (field[i] == '.' && !dot) { dot = true; continue; }
        return false;
    }
    return digits;
}

// The four leading characters spreadsheets treat as the start of a formula,
// plus the two whitespace characters that let an attacker hide one behind
// them.
inline bool csvFieldNeedsNeutralising(const char *field, size_t len) {
    if (field == nullptr || len == 0) return false;
    const char c = field[0];
    const bool trigger = c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' || c == '\r';
    return trigger && !csvFieldIsNumber(field, len);
}

// Rewrites one CSV line into `out`, prefixing every formula-triggering field
// with a single quote inside its quotes. Quoting and embedded commas are
// preserved exactly; a field that needs neutralising but was not quoted
// becomes quoted, because the leading ' must not be mistaken for data.
//
// Reports success separately from length, unlike the other formatters'
// "0 means refused": a blank line legitimately renders as zero bytes, and an
// export that silently dropped blank lines would not be a faithful copy.
inline bool csvSafeLine(const char *in, char *out, size_t outSize, size_t &written) {
    written = 0;
    if (in == nullptr || out == nullptr || outSize == 0) return false;
    size_t w = 0;
    const char *p = in;

    auto put = [&](char c) -> bool {
        if (w + 1 >= outSize) return false;
        out[w++] = c;
        return true;
    };

    for (;;) {
        // One field: either "quoted, with "" escapes" or bare up to the comma.
        const bool quoted = *p == '"';
        const char *body = quoted ? p + 1 : p;
        const char *end = body;
        if (quoted) {
            while (*end != '\0') {
                if (*end == '"' && end[1] == '"') { end += 2; continue; }
                if (*end == '"') break;
                end++;
            }
        } else {
            while (*end != '\0' && *end != ',') end++;
        }

        // The first character as a spreadsheet would see it: a doubled quote
        // renders as one, so compare against the decoded first character.
        char firstDecoded = *body;
        if (quoted && body[0] == '"' && body[1] == '"') firstDecoded = '"';
        const size_t bodyLen = (size_t)(end - body);
        char probe[2] = {firstDecoded, '\0'};
        const bool neutralise =
            bodyLen > 0 && csvFieldNeedsNeutralising(probe, 1) &&
            !csvFieldIsNumber(body, bodyLen);

        if (neutralise || quoted) {
            if (!put('"')) return false;
        }
        if (neutralise && !put('\'')) return false;
        for (const char *q = body; q < end; ++q) {
            if (!put(*q)) return false;
        }
        if (neutralise || quoted) {
            if (!put('"')) return false;
        }

        p = quoted ? (*end == '"' ? end + 1 : end) : end;
        if (*p != ',') break;
        if (!put(',')) return false;
        p++;
    }
    // Anything trailing a closing quote (there should be nothing) is copied
    // rather than dropped: an export must not quietly lose bytes.
    for (; *p != '\0'; ++p) {
        if (!put(*p)) return false;
    }
    out[w] = '\0';
    written = w;
    return true;
}
