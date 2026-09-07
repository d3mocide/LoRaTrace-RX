#pragma once
// LoRaTrace RX — the immutable per-run provenance record.
//
// A run directory's CSVs record what was heard; nothing in them records what
// the device *was* while hearing it (audit A18). The boot banner names a
// build, but the boot banner is not on the card, and the settings files at
// the SD root can change after a run ends — so a folder copied off the card
// months later could not answer which firmware, which region, which modem
// parameters, or even how many separate boots produced it.
//
// manifest.txt answers that. It is append-only: one `[session]` block per
// boot that opens this run directory, because the logger deliberately
// rejoins the run it left when a card is reseated mid-drive, and those boots
// are different measurement sessions even though they share a folder name
// (audit A25).
//
// Pure formatter, host-tested; the SD half is logger_task's.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Bump when a field changes meaning or is removed. Appending a new key is
// not a breaking change — importers must read by name and ignore unknowns.
constexpr uint16_t RUN_MANIFEST_SCHEMA = 1;

// 128 bits, rendered as 32 hex characters. Distinguishes two boots that
// share a run number, and survives a run number that collided with an
// unrelated run on a replacement card.
constexpr size_t RUN_SESSION_ID_LEN = 32;
constexpr size_t RUN_SESSION_ID_BUF = RUN_SESSION_ID_LEN + 1;

struct RunManifestSession {
    const char *session_id = "";     // RUN_SESSION_ID_LEN hex chars
    const char *firmware_version = "";
    const char *build_rev = "";      // git short SHA, "-dirty" when modified
    const char *board = "";
    const char *radio = "";
    const char *profile = "";        // mission profile at the time of writing
    const char *region = "";
    const char *timestamp_utc = "";  // empty before the GPS has a date
    uint16_t run = 0;
    uint16_t schema = RUN_MANIFEST_SCHEMA;
    uint32_t uptime_ms = 0;
    // The resolved modem parameters this session started on. Recorded here
    // because config.txt at the SD root can be edited after the run.
    float freq_mhz = 0.0f;
    float bw_khz = 0.0f;
    uint8_t sf = 0;
    uint8_t cr_denom = 0;
    uint8_t sync_word = 0;
    uint16_t capture_window_ms = 0;
    int16_t sweep_margin_dbm_x10 = 0;
    bool sd_recovered = false; // this session rejoined an existing directory
};

// Renders one session block. Returns characters written excluding the NUL,
// or 0 on truncation — same "0 means don't write this" contract the CSV
// formatters use.
//
// `timestamp_semantics` is spelled out rather than assumed: every *_millis
// column in this run is device uptime, not wall clock, and the mapping to
// UTC is only as good as the GPS fix that supplied it.
inline size_t runManifestFormatSession(const RunManifestSession &s, char *out, size_t outSize) {
    if (out == nullptr || outSize == 0) return 0;
    const int n = snprintf(
        out, outSize,
        "[session]\n"
        "schema=%u\n"
        "session_id=%s\n"
        "run=%u\n"
        "started_utc=%s\n"
        "started_uptime_ms=%lu\n"
        "rejoined_existing_run=%u\n"
        "firmware_version=%s\n"
        "build_rev=%s\n"
        "board=%s\n"
        "radio=%s\n"
        "profile=%s\n"
        "region=%s\n"
        "freq_mhz=%.6f\n"
        "sf=%u\n"
        "bw_khz=%.2f\n"
        "cr_denom=%u\n"
        "sync_word=0x%02X\n"
        "capture_window_ms=%u\n"
        "sweep_margin_dbm_x10=%d\n"
        "timestamp_semantics=rx_millis is device uptime; utc from GPS when present\n"
        "position_semantics=receiver position at reception, never the transmitter\n",
        (unsigned)s.schema, s.session_id ? s.session_id : "", (unsigned)s.run,
        s.timestamp_utc ? s.timestamp_utc : "", (unsigned long)s.uptime_ms,
        (unsigned)s.sd_recovered, s.firmware_version ? s.firmware_version : "",
        s.build_rev ? s.build_rev : "", s.board ? s.board : "", s.radio ? s.radio : "",
        s.profile ? s.profile : "", s.region ? s.region : "", (double)s.freq_mhz,
        (unsigned)s.sf, (double)s.bw_khz, (unsigned)s.cr_denom, (unsigned)s.sync_word,
        (unsigned)s.capture_window_ms, (int)s.sweep_margin_dbm_x10);
    if (n < 0 || (size_t)n >= outSize) return 0;
    return (size_t)n;
}

// Renders a 128-bit id as lowercase hex. `randomByte` supplies the entropy,
// so the generator is testable without hardware.
inline void runSessionIdGenerate(char *out, size_t outSize, uint8_t (*randomByte)()) {
    if (out == nullptr || outSize < RUN_SESSION_ID_BUF || randomByte == nullptr) return;
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < RUN_SESSION_ID_LEN; i += 2) {
        const uint8_t byte = randomByte();
        out[i] = DIGITS[byte >> 4];
        out[i + 1] = DIGITS[byte & 0x0F];
    }
    out[RUN_SESSION_ID_LEN] = '\0';
}

inline bool runSessionIdIsValid(const char *id) {
    if (id == nullptr) return false;
    size_t len = 0;
    for (; id[len] != '\0'; ++len) {
        if (len >= RUN_SESSION_ID_LEN) return false;
        const char c = id[len];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return len == RUN_SESSION_ID_LEN;
}
