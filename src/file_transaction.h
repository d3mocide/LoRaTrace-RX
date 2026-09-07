#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Caller owns the SD bus. Preserve the last complete file until a verified
// replacement exists; recovery deliberately never promotes an unfinished .tmp.
template<class FS> bool recoverTextFile(FS &fs, const char *path) {
    char backup[96];
    int n = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (n < 0 || (size_t)n >= sizeof(backup)) return false;
    if (fs.exists(path)) return true;
    return fs.exists(backup) && fs.rename(backup, path);
}

template<class FS> bool replaceTextFile(FS &fs, const char *path, const char *data, size_t len) {
    if (!data || len == 0) return false;
    char temp[96], backup[96];
    int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
    int b = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (n < 0 || b < 0 || (size_t)n >= sizeof(temp) || (size_t)b >= sizeof(backup)) return false;
    recoverTextFile(fs, path);
    if (fs.exists(temp) && !fs.remove(temp)) return false;
    auto f = fs.open(temp, "w");
    if (!f) return false;
    const bool written = f.write((const uint8_t *)data, len) == len;
    f.close();
    if (!written) return false;
    f = fs.open(temp, "r");
    if (!f) return false;
    bool verified = f.size() == len;
    for (size_t i = 0; verified && i < len; ++i) verified = f.read() == (uint8_t)data[i];
    f.close();
    if (!verified) return false;
    if (fs.exists(backup) && !fs.remove(backup)) return false;
    if (fs.exists(path) && !fs.rename(path, backup)) return false;
    if (!fs.rename(temp, path)) { recoverTextFile(fs, path); return false; }
    return true;
}

// Reject overlong input as a whole, not a valid-looking truncated prefix.
template<class File> bool readBoundedLine(File &file, char *out, size_t capacity) {
    size_t used = 0;
    bool overflow = false;
    while (file.available()) {
        const int c = file.read();
        if (c < 0 || c == '\n') break;
        if (used + 1 < capacity) out[used++] = (char)c;
        else overflow = true;
    }
    if (capacity) out[overflow ? 0 : used] = '\0';
    return capacity > 0 && !overflow;
}

// What ensureCsvFile() found. A CSV whose header cannot be verified is
// never appended to: an unknown schema silently reinterprets every later
// column, and "the file already exists" is not evidence it is ours.
enum class CsvFileState : uint8_t { FAILED, READY, CREATED, REPAIRED, REPLACED };

template<class FS> bool writeCsvHeader(FS &fs, const char *path, const char *header, size_t len) {
    auto f = fs.open(path, "w");
    if (!f) return false;
    const bool ok = f.write((const uint8_t *)header, len) == len &&
                    f.write((const uint8_t *)"\n", 1) == 1;
    f.close();
    return ok;
}

template<class FS> CsvFileState ensureCsvFile(FS &fs, const char *path, const char *header) {
    const size_t headerLen = strlen(header);
    auto f = fs.open(path, "r");
    if (f && f.size() > 0) {
        bool valid = true;
        for (size_t i = 0; valid && i < headerLen; ++i) valid = f.read() == (uint8_t)header[i];
        int end = valid ? f.read() : -1;
        if (end == '\r') end = f.read();
        valid = valid && end == '\n';
        const bool terminated = valid && f.seek(f.size() - 1) && f.read() == '\n';
        f.close();
        if (terminated) return CsvFileState::READY;
        if (valid) {
            // A power cut or a short write leaves a partial last row. Close
            // it so the next append starts a fresh line instead of merging
            // into it; the stub stays visible as a short row rather than
            // being silently repaired into plausible-looking evidence.
            auto a = fs.open(path, "a");
            if (!a) return CsvFileState::FAILED;
            const bool ok = a.write((const uint8_t *)"\n", 1) == 1;
            a.close();
            return ok ? CsvFileState::REPAIRED : CsvFileState::FAILED;
        }
        char aside[96];
        const int n = snprintf(aside, sizeof(aside), "%s.bad", path);
        if (n < 0 || (size_t)n >= sizeof(aside)) return CsvFileState::FAILED;
        if (fs.exists(aside) && !fs.remove(aside)) return CsvFileState::FAILED;
        if (!fs.rename(path, aside)) return CsvFileState::FAILED;
        return writeCsvHeader(fs, path, header, headerLen) ? CsvFileState::REPLACED
                                                           : CsvFileState::FAILED;
    }
    f.close();
    return writeCsvHeader(fs, path, header, headerLen) ? CsvFileState::CREATED
                                                       : CsvFileState::FAILED;
}
