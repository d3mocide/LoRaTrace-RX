#pragma once
// LoRaTrace RX — run identity: one wardrive, one directory on the card.
//
// Before this, both logs were single append-only files that every power-on
// added to forever. That is durable but it is not usable: a drive is the
// unit an operator actually thinks in ("last night's run"), and one
// continuous file makes the obvious operations — share a drive, import a
// drive, delete a drive, diff two drives — into text-editing chores.
//
// So each run gets `/loratrace/runNNNN/` holding its own `detections.csv`
// and `session.csv`. Copy the folder, you have the run.
//
// **Why an index and not a timestamp.** The name has to be decided at the
// moment logging starts, and at that moment the device does not know what
// time it is: absolute time comes from GPS, this board has no verified RTC,
// and a cold time-to-first-fix is tens of seconds at best. Naming by
// timestamp would mean either delaying the file (losing every packet heard
// during TTFF) or renaming it later (leaving a provisional name behind on
// any power cut before the rename). A monotonic index is knowable
// immediately, needs no clock, and is stable under power loss. The wall
// clock still reaches the card — it is recorded *inside* the run, on the
// first health row that has a fix, which is enough to date the run
// afterwards without ever having gated its creation on a clock.
//
// The next index is derived by scanning the card rather than stored in a
// counter file: the directory listing is the truth, it cannot drift out of
// sync with reality, and there is no mutable state to corrupt on a power
// cut mid-write. Scanning for the *highest* index (not the first gap) means
// a deleted run is never silently reused by a later one.
//
// Pure logic, no Arduino/FreeRTOS, so `pio test -e native` covers the
// naming and parsing that the on-card layout depends on.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 4 digits keeps a run directory inside FAT's 8.3 short-name budget, so the
// folder is readable even on a host that doesn't do long names.
constexpr uint16_t RUN_INDEX_MAX = 9999;
constexpr const char *RUN_DIR_PREFIX = "run";
constexpr size_t RUN_PATH_MAX = 64; // "/loratrace/run0007/detections.csv" + slack

// The ESP32 SD library has returned both bare names and full paths from
// File::name() across core versions, so never trust which one you're
// holding — reduce to the last path component first.
inline const char *runBaseName(const char *path) {
    if (path == nullptr) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Parses a run directory name to its index. Returns 0 — never a valid run
// index — for anything that isn't exactly `runNNNN`, which is how
// config.txt, the legacy top-level logs, and any stray file a user dropped
// on the card get skipped by the scan rather than confusing it.
inline uint16_t runIndexFromName(const char *name) {
    const char *base = runBaseName(name);
    const size_t prefixLen = strlen(RUN_DIR_PREFIX);
    if (strncmp(base, RUN_DIR_PREFIX, prefixLen) != 0) return 0;

    const char *digits = base + prefixLen;
    // Exactly four digits and nothing after: "run7", "run00007" and
    // "run0007.bak" are all rejected rather than guessed at.
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        if (digits[i] < '0' || digits[i] > '9') return 0;
        value = value * 10 + (uint32_t)(digits[i] - '0');
    }
    if (digits[4] != '\0') return 0;
    return (uint16_t)value;
}

// Builds "<root>/runNNNN". Returns characters written, or 0 if the buffer
// is too small — callers must treat 0 as "don't touch the card", since a
// truncated path would write a run's data into the wrong place.
inline size_t runDirPath(char *out, size_t outSize, const char *root, uint16_t index) {
    if (out == nullptr || outSize == 0 || root == nullptr) return 0;
    int n = snprintf(out, outSize, "%s/%s%04u", root, RUN_DIR_PREFIX, (unsigned)index);
    if (n < 0 || (size_t)n >= outSize) return 0;
    return (size_t)n;
}

// Builds "<root>/runNNNN/<leaf>", same contract as runDirPath().
inline size_t runFilePath(char *out, size_t outSize, const char *root, uint16_t index,
                          const char *leaf) {
    if (out == nullptr || outSize == 0 || root == nullptr || leaf == nullptr) return 0;
    int n = snprintf(out, outSize, "%s/%s%04u/%s", root, RUN_DIR_PREFIX, (unsigned)index, leaf);
    if (n < 0 || (size_t)n >= outSize) return 0;
    return (size_t)n;
}

// Given the highest index already on the card, the index this run should
// use. Saturates rather than wrapping: at 9999 runs, continuing to append
// to the last run is a far better failure than silently overwriting run
// 0001's data, and the run column in both CSVs makes the saturation
// visible in the data rather than hiding it.
inline uint16_t runNextIndex(uint16_t highestSeen) {
    return highestSeen < RUN_INDEX_MAX ? (uint16_t)(highestSeen + 1) : RUN_INDEX_MAX;
}
