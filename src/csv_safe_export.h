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

// True when the whole field parses as a plain negative decimal number.
// Deliberately narrow: it exists only so the '-' exemption below can be safe,
// and half the columns in these files are negative dBm readings.
//
// A leading '+' is NOT exempted, even though "+8" is arithmetically a number.
// No formatter in this firmware emits one — printf writes no sign for positive
// values unless asked — so a '+'-leading field can only have come from
// radio-supplied text, and Excel does enter "+8" in formula mode. Surfaced by
// fuzzing (audit A27).
inline bool csvFieldIsNumber(const char *field, size_t len) {
    if (field == nullptr || len == 0) return false;
    size_t i = 0;
    if (field[i] == '-') i++;
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

        // Adding quotes to a field that did not have them means taking
        // responsibility for escaping what is inside it. Copying the body
        // verbatim produced broken quoting whenever an unquoted field held a
        // '"' — the row desynchronised and later fields escaped neutralisation
        // (found by fuzzing, audit A27). A field that arrived quoted is
        // already escaped by whoever wrote it, so it is copied as-is.
        const bool addingQuotes = neutralise && !quoted;
        if (neutralise || quoted) {
            if (!put('"')) return false;
        }
        if (neutralise && !put('\'')) return false;
        for (const char *q = body; q < end; ++q) {
            if (addingQuotes && *q == '"' && !put('"')) return false;
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
    // A field must end at a comma or at the end of the line. Anything else
    // means the input is not CSV this function can reason about — and the
    // earlier version copied that remainder through verbatim, which walked
    // straight past neutralisation: `""=,@",,,` came out with an unquoted
    // field starting with '@' (found by fuzzing, audit A27).
    //
    // Refused rather than repaired. The caller's contract is "safe to open in
    // a spreadsheet", and a row this function cannot parse is a row it cannot
    // promise that for; streamSafeCsvFile() turns a refusal into a failed
    // export rather than a file that looks complete.
    if (*p != '\0') return false;
    out[w] = '\0';
    written = w;
    return true;
}
