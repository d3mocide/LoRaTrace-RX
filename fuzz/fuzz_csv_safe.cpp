// Audit A27/A12: the spreadsheet-safe export transform, fed arbitrary bytes.
// Its whole job is a security property, and it runs over a CSV line that
// already contains radio-supplied names.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/csv_safe_export.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > CSV_SAFE_LINE_MAX) return 0;
    char line[CSV_SAFE_LINE_MAX + 1];
    memcpy(line, data, size);
    line[size] = '\0';

    static char out[CSV_SAFE_LINE_MAX + 64];
    for (size_t cap = 0; cap <= size + 8 && cap <= sizeof(out) - 32; cap++) {
        memset(out, 0xAA, sizeof(out));
        size_t written = 0;
        const bool ok = csvSafeLine(line, out, cap, written);
        if (!ok) {
            if (written != 0) __builtin_trap(); // refusal must report nothing written
            continue;
        }
        if (written >= cap) __builtin_trap();
        if (out[written] != '\0') __builtin_trap();
        if (strlen(out) != written) __builtin_trap();
        for (size_t i = cap; i < sizeof(out); i++) {
            if ((uint8_t)out[i] != 0xAA) __builtin_trap();
        }
        // The security property: no *unquoted* field may begin with a
        // formula trigger. '-' is excluded deliberately — negative dBm values
        // are legal and must not be neutralised; the unit tests cover that
        // half, which needs numeric parsing this walker does not do.
        bool inQuotes = false, atFieldStart = true;
        for (size_t i = 0; i < written; i++) {
            const char c = out[i];
            if (inQuotes) {
                if (c != '"') continue;
                if (i + 1 < written && out[i + 1] == '"') { i++; continue; }
                inQuotes = false;
                continue;
            }
            if (atFieldStart) {
                // An empty field: the comma ends it and the next one starts
                // here, so field-start survives. Consuming it as content was a
                // bug in this checker, not in the transform.
                if (c == ',') continue;
                if (c == '"') { inQuotes = true; atFieldStart = false; continue; }
                if (c == '=' || c == '+' || c == '@' || c == '\t' || c == '\r') {
                    __builtin_trap(); // unquoted formula trigger survived
                }
                atFieldStart = false;
                continue;
            }
            if (c == ',') atFieldStart = true;
        }
    }
    return 0;
}
