#include "wifi_task.h"

#include <Arduino.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h> // strtol, for the hex-or-decimal sync_word arg below
#include <string.h> // strcmp/strncpy

#include "battery.h"
#include "channel_config.h"
#include "ap_credential.h"
#include "csv_safe_export.h"
#include "file_transaction.h"
#include "config.h"
#include "display_settings.h"
#include "gps_task.h"
#include "logger_task.h"
#include "memory_stats.h"
#include "radio_task.h"
#include "run_log.h"
#include "serial_lock.h"
#include "spi_bus.h"
#include "version.h"
#include "web_assets.h"

namespace {


WebServer server(80);
volatile bool apRequested = false;
bool apActive = false;

// Per-AP-session anti-CSRF token. A browser that can reach this AP will
// happily submit an attacker page's form to it; the response is unreadable
// cross-origin, but the write still lands (audit A11). Regenerated on every
// AP start, so a token never outlives the session it was issued for.
char csrfToken[33] = {0};

void csrfTokenRegenerate() {
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(csrfToken) - 1; i++) {
        csrfToken[i] = DIGITS[esp_random() & 0x0F];
    }
    csrfToken[sizeof(csrfToken) - 1] = '\0';
}

// Computed once and cached rather than recomputed on every use — a real
// hardware run showed the AP-started log line print with the SSID missing
// once. No definitive root cause found (ESP.getEfuseMac() is deterministic
// and nothing should touch a local buffer between fill and print), but one
// long-lived buffer, filled once, removes the whole category of doubt.
char cachedSsid[32] = {0};

// False when the key could not be written to the card: it still protects
// this session, but the operator must be told it will not survive a reboot.
bool apKeyPersisted = false;

const char *ssidCached() {
    if (cachedSsid[0] == '\0') {
        // Chip-unique suffix (efuse MAC) so multiple LoRaTrace units nearby
        // don't collide on the same SSID — a real scenario for a wardrive.
        const uint64_t mac = ESP.getEfuseMac();
        snprintf(cachedSsid, sizeof(cachedSsid), "LoRaTrace-%04X", (unsigned)(mac & 0xFFFFu));
    }
    return cachedSsid;
}

// How long a handler waits for the shared SPI bus. Bounded, matching every
// other non-radio SD caller (logger_task.cpp's BUS_WAIT) — an HTTP request
// isn't worth stalling indefinitely for; the client gets a 503 to retry.
constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(2000);

// Bytes per chunk when streaming a CSV off SD. Small enough each lock hold
// is brief (must never make the radio task wait), large enough to avoid
// per-chunk open/seek/close overhead.
constexpr size_t CSV_CHUNK_SIZE = 512;

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

const char *discoveryStateName(DiscoverySweepState state) {
    switch (state) {
        case DiscoverySweepState::RUNNING: return "RUNNING";
        case DiscoverySweepState::COMPLETE: return "COMPLETE";
        case DiscoverySweepState::CANCELLED: return "CANCELLED";
        case DiscoverySweepState::FAILED: return "FAILED";
        default: return "IDLE";
    }
}

const char *energyStateName(EnergySweepState state) {
    switch (state) {
        case EnergySweepState::RUNNING: return "RUNNING";
        case EnergySweepState::COMPLETE: return "COMPLETE";
        case EnergySweepState::CANCELLED: return "CANCELLED";
        case EnergySweepState::FAILED: return "FAILED";
        default: return "IDLE";
    }
}

void handleStatus() {
    GpsFix fix = {};
    const bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(100));
    const bool positioned = haveFix && fix.has_position;
    const ChannelParams home = radioActiveChannel();
    const EnergyStrongestPeak strongest = radioEnergyStrongestPeak();

    // The browser only receives radio-owned snapshots. It can observe the
    // bounded acquisition state, but never retunes or polls the SX1262.
    char json[1280];
    const int n = snprintf(
        json, sizeof(json),
        "{"
        "\"firmware_version\":\"%s\",\"profile\":\"%s\",\"home_freq_mhz\":%.3f,"
        "\"rx\":%lu,\"crc_err\":%lu,\"queue_drop\":%lu,\"bus_miss\":%lu,"
        "\"rows\":%lu,\"row_drop\":%lu,\"flushes\":%lu,\"max_flush_ms\":%lu,\"max_session_ms\":%lu,"
        "\"sd_ready\":%s,\"session_rows\":%lu,\"run\":%u,"
        "\"nmea\":%lu,\"nmea_bad_crc\":%lu,"
        "\"has_fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"sats\":%u,\"sats_in_view\":%u,"
        "\"heap_free\":%lu,\"heap_min\":%lu,\"batt_mv\":%lu,\"wifi_clients\":%u,"
        "\"trace_paused\":%s,"
        "\"probe\":{\"state\":\"%s\",\"index\":%u,\"count\":%u,\"cad_free\":%u,"
        "\"cad_detected\":%u,\"cad_timeout\":%u,\"errors\":%u},"
        "\"sweep\":{\"state\":\"%s\",\"repeat_active\":%s,\"repeat_count\":%lu,"
        "\"bin_index\":%u,\"bin_count\":%u,\"peaks\":%u,\"strongest_valid\":%s,"
        "\"strongest_freq_mhz\":%.3f,\"strongest_rssi_dbm\":%.1f,"
        "\"pass_b_attempts\":%lu,\"pass_b_detections\":%lu}"
        "}",
        FIRMWARE_VERSION, missionProfileName((uint8_t)radioActiveProfile()), (double)home.freq_mhz,
        (unsigned long)radioPacketCount(), (unsigned long)radioCrcErrorCount(),
        (unsigned long)radioQueueDropCount(), (unsigned long)radioBusMissCount(),
        (unsigned long)loggerRowsWritten(), (unsigned long)loggerRowsDropped(),
        (unsigned long)loggerFlushCount(), (unsigned long)loggerMaxFlushMs(),
        (unsigned long)loggerMaxSessionMs(), loggerSdReady() ? "true" : "false",
        (unsigned long)loggerSessionRows(), (unsigned)loggerRunIndex(),
        (unsigned long)gpsSentenceCount(), (unsigned long)gpsChecksumErrorCount(),
        positioned ? "true" : "false", positioned ? fix.lat : 0.0, positioned ? fix.lon : 0.0,
        (unsigned)fix.satellites, (unsigned)fix.sats_in_view, (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(), (unsigned long)batteryMilliVolts(), (unsigned)wifiClientCount(),
        radioIsTracePaused() ? "true" : "false",
        discoveryStateName(radioDiscoverySweepState()), (unsigned)radioDiscoveryCandidateIndex(),
        (unsigned)radioDiscoveryCandidateCount(), (unsigned)radioDiscoveryCadFreeCount(),
        (unsigned)radioDiscoveryCadDetectedCount(), (unsigned)radioDiscoveryCadTimeoutCount(),
        (unsigned)radioDiscoveryErrorCount(), energyStateName(radioEnergySweepState()),
        radioEnergySweepRepeatIsActive() ? "true" : "false",
        (unsigned long)radioEnergySweepRepeatCount(), (unsigned)radioEnergyBinIndex(),
        (unsigned)radioEnergyBinCount(), (unsigned)radioEnergyPeakCount(),
        strongest.valid ? "true" : "false", (double)strongest.freq_mhz,
        (double)strongest.rssi_peak_dbm_x10 / 10.0, (unsigned long)radioPassBAttemptCount(),
        (unsigned long)radioPassBDetectionCount());

    if (n < 0 || (size_t)n >= sizeof(json)) {
        server.send(500, "text/plain", "status too large");
        return;
    }
    server.send(200, "application/json", json);
}

// Walks /loratrace for runNNNN directories, same shape as
// logger_task.cpp's highestRunIndexLocked() but collecting every valid
// index. Kept as wifi_task's own copy rather than exported from
// logger_task.h to avoid sharing mutable file-scope state across
// translation units.
// Fixed stack buffer + snprintf, not String — String's `+=` in a loop
// reallocates/copies on every growth, real heap churn/fragmentation risk
// over a long AP session. ~340 runs fit before the defensive cap kicks in;
// a truncated run list beats a buffer overrun, and detections.csv is still
// reachable directly off the SD card either way.
void handleRuns() {
    char json[2048];
    size_t pos = 0;
    json[pos++] = '[';
    bool first = true;

    SpiBusLock lock(BUS_WAIT);
    if (lock.held()) {
        File dir = SD.open(CHANNEL_CONFIG_DIR);
        if (dir) {
            for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
                const uint16_t idx = runIndexFromName(entry.name());
                entry.close();
                if (idx == 0) continue;
                // Leave room for the trailing "]\0" even after this entry.
                if (pos + 8 >= sizeof(json)) break;
                if (!first) json[pos++] = ',';
                pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "%u", (unsigned)idx);
                first = false;
            }
            dir.close();
        }
    }
    json[pos++] = ']';
    json[pos] = '\0';
    server.send(200, "application/json", json);
}

// Streams one allowlisted CSV from a run directory.
// as a chunked download. The SPI bus lock is acquired fresh per chunk and
// released before the slow part (writing to the TCP socket), mirroring
// logger_task.cpp's appendToFile() discipline — never hold the bus across
// anything that isn't a single bounded SD operation, since holding it for
// a whole large-file transfer would stall the radio task.
void streamCsvFile(const char *path, const char *downloadName) {
    memoryStatsLog("csv-download-before");
    size_t fileSize = 0;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            server.send(503, "text/plain", "SD busy, try again");
            return;
        }
        // Read mode is SD.open()'s default — matches config.cpp's own read
        // path, which relies on the same default rather than an explicit
        // FILE_READ (unverified in this codebase; not worth the risk here).
        File f = SD.open(path);
        if (!f) {
            server.send(404, "text/plain", "not found");
            return;
        }
        fileSize = f.size();
        f.close();
    }

    // char buffer + snprintf, not chained String allocations — same
    // heap-churn reasoning as handleRuns() above.
    char disposition[64];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", downloadName);
    server.sendHeader("Content-Disposition", disposition);
    server.setContentLength(fileSize);
    server.send(200, "text/csv", "");

    uint8_t buf[CSV_CHUNK_SIZE];
    size_t offset = 0;
    bool complete = false;
    while (offset < fileSize) {
        // Never more than the length already declared. A run file this AP is
        // serving is usually still being appended to, and reading a full
        // chunk regardless of what remains sent more bytes than the
        // Content-Length promised (audit A24).
        const size_t want =
            (fileSize - offset) < sizeof(buf) ? (fileSize - offset) : sizeof(buf);
        // File::read() returns int and can be negative on error — read into
        // a signed local first. Assigning a -1 error straight into a size_t
        // would turn "read failed" into "read 4 billion bytes" and send
        // garbage from `buf` under a bogus huge length.
        int readLen = 0;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) break; // bus busy — client gets a short/incomplete file, not a stall
            File f = SD.open(path); // read mode, same default as above
            // An unchecked seek reads from wherever the handle happened to
            // be, which would repeat or skip a section of the file silently.
            if (!f || !f.seek(offset)) {
                f.close();
                break;
            }
            readLen = f.read(buf, want);
            f.close();
        }
        if (readLen <= 0) break;
        WiFiClient client = server.client();
        if (!client.connected()) break;
        const size_t written = client.write(buf, (size_t)readLen);
        if (written != (size_t)readLen) break;
        offset += written;
        complete = offset >= fileSize;

        // An operator turning the AP off should not have to wait out a
        // multi-megabyte transfer; the download is abandoned, not finished.
        if (!complete && wifiShutdownRequested()) break;

        // WiFiClient::write() can spend multiple seconds retrying a full TCP
        // socket. Yield between successful chunks so the Core 0 idle task can
        // service the watchdog during a large SD download.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!complete) {
        // Abort rather than return: the body is short of the Content-Length
        // already sent, and a client must see a broken transfer instead of a
        // file that looks whole (audit A24).
        WiFiClient client = server.client();
        client.stop();
    }
    memoryStatsLog("csv-download-after");
}

// Matches "/api/runs/<n>/<known.csv>". Hand-parsed
// rather than relying on WebServer's path-pattern support, since that's a
// version-specific feature this codebase's pinned core (2.0.17) shouldn't
// be assumed to have — and the shape here is small and fixed anyway.
// Same file, rendered so a spreadsheet cannot execute a node name that
// arrived over the air (csv_safe_export.h, audit A12). Two passes: the
// transformed length is not knowable without doing the transform, and a
// Content-Length that disagrees with the body is its own bug. The canonical
// file on the card is never touched — this is a second view of it.
void streamSafeCsvFile(const char *path, const char *downloadName) {
    char line[CSV_SAFE_LINE_MAX];
    char safe[CSV_SAFE_LINE_MAX];
    size_t total = 0;
    bool measured = false;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            server.send(503, "text/plain", "SD busy, try again");
            return;
        }
        File f = SD.open(path);
        if (!f) {
            server.send(404, "text/plain", "not found");
            return;
        }
        measured = true;
        while (f.available()) {
            size_t written = 0;
            // A line too long to read whole, or too long once neutralised,
            // makes the export incomplete — better to refuse than to ship a
            // file with a row quietly missing.
            if (!readBoundedLine(f, line, sizeof(line)) ||
                !csvSafeLine(line, safe, sizeof(safe), written)) {
                measured = false;
                break;
            }
            total += written + 1; // the newline this line will carry
        }
        f.close();
    }
    if (!measured) {
        server.send(500, "text/plain", "row too long for a safe export");
        return;
    }

    char disposition[64];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", downloadName);
    server.sendHeader("Content-Disposition", disposition);
    server.setContentLength(total);
    server.send(200, "text/csv", "");

    size_t sent = 0;
    {
        SpiBusLock lock(BUS_WAIT);
        File f = lock.held() ? SD.open(path) : File();
        while (f && f.available()) {
            size_t written = 0;
            if (!readBoundedLine(f, line, sizeof(line)) ||
                !csvSafeLine(line, safe, sizeof(safe), written)) {
                break;
            }
            safe[written] = '\n';
            WiFiClient client = server.client();
            if (!client.connected()) break;
            if (client.write((const uint8_t *)safe, written + 1) != written + 1) break;
            sent += written + 1;
        }
        if (f) f.close();
    }
    if (sent != total) {
        // Short of the declared length: abort so the client sees a broken
        // transfer rather than a file that looks whole (same rule as A24).
        WiFiClient client = server.client();
        client.stop();
    }
}

// The seven files a run directory holds. cell.csv and focus.csv were missing
// from the allowlist and the page, so two of the run's own outputs could not
// be retrieved over the AP at all (audit A24).
const char *const RUN_CSV_LEAVES[] = {
    "detections.csv", "session.csv", "probe.csv", "energy.csv",
    "nodes.csv",      "cell.csv",    "focus.csv",
};

bool runCsvLeafAllowed(const char *leaf) {
    for (const char *known : RUN_CSV_LEAVES) {
        if (strcmp(leaf, known) == 0) return true;
    }
    return false;
}

void handleNotFound() {
    const String uri = server.uri();
    if (uri.startsWith("/api/runs/")) {
        int idx = 0;
        char leaf[32] = {0};
        // run_log.h numbers runs 1..RUN_INDEX_MAX; anything else is not a run
        // this device could have written, and must not be narrowed into one.
        if (sscanf(uri.c_str(), "/api/runs/%d/%31s", &idx, leaf) == 2 && idx > 0 &&
            idx <= (int)RUN_INDEX_MAX) {
            // "<leaf>?safe" asks for the spreadsheet-safe rendering. sscanf's
            // %s stops at whitespace, not '?', so the query rides in `leaf`.
            char *query = strchr(leaf, '?');
            const bool wantSafe = query != nullptr && strcmp(query, "?safe") == 0;
            if (query != nullptr) *query = '\0';
            if (!runCsvLeafAllowed(leaf)) {
                server.send(404, "text/plain", "not found");
                return;
            }
            char path[RUN_PATH_MAX];
            if (runFilePath(path, sizeof(path), CHANNEL_CONFIG_DIR, (uint16_t)idx, leaf) > 0) {
                char downloadName[56];
                snprintf(downloadName, sizeof(downloadName), "run%04u_%s%s", (unsigned)idx,
                         wantSafe ? "safe_" : "", leaf);
                if (wantSafe) {
                    streamSafeCsvFile(path, downloadName);
                } else {
                    streamCsvFile(path, downloadName);
                }
                return;
            }
        }
    }
    server.send(404, "text/plain", "not found");
}

// Returns whichever profile name matches `arg`, or false if it's neither —
// callers reject the request rather than guessing.
bool parseProfileArg(const String &arg, MissionProfile &profile) {
    if (arg == "meshtastic") {
        profile = MissionProfile::MESHTASTIC;
        return true;
    }
    if (arg == "meshcore") {
        profile = MissionProfile::MESHCORE;
        return true;
    }
    return false;
}

// WebServer's String::toInt() intentionally accepts a non-numeric string as
// zero, which is fine for its existing RF form only because config.cpp does
// the definitive validation.  The small preference endpoints below need to
// distinguish a real zero (idle dim off) from malformed input at the HTTP
// boundary, so parse their finite integer domain strictly here.
bool parseUint8Arg(const char *name, uint8_t min, uint8_t max, uint8_t &value) {
    if (!server.hasArg(name)) return false;
    const String raw = server.arg(name);
    if (raw.length() == 0) return false;
    char *end = nullptr;
    const long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0' || parsed < min || parsed > max) return false;
    value = (uint8_t)parsed;
    return true;
}

// A cross-origin form can be submitted, but it cannot read /api/session's
// reply to learn the token, and browsers that do send Origin on a POST are
// checked against the AP's own host too. Neither check alone is enough:
// Origin is absent on some requests, and CORS does not prevent the write.
bool originIsSelf() {
    if (!server.hasHeader("Origin")) return true; // absent, not forged
    const String origin = server.header("Origin");
    if (origin == "null") return false;
    const String host = server.hostHeader();
    return host.length() > 0 && origin.endsWith(host) &&
           (origin.startsWith("http://") || origin.startsWith("https://"));
}

// Answers the request itself and returns false when a state change must not
// proceed. Every POST handler starts with this.
bool stateChangeAllowed() {
    if (!originIsSelf()) {
        server.send(403, "application/json", "{\"ok\":false,\"error\":\"bad origin\"}");
        return false;
    }
    const String supplied =
        server.hasHeader("X-CSRF-Token") ? server.header("X-CSRF-Token") : server.arg("csrf");
    if (csrfToken[0] == '\0' || supplied != csrfToken) {
        server.send(403, "application/json", "{\"ok\":false,\"error\":\"reload the page\"}");
        return false;
    }
    return true;
}

// The token this AP session accepts. A cross-origin caller can trigger this
// request but cannot read what it returns, which is what makes it a secret.
void handleSession() {
    char json[64];
    snprintf(json, sizeof(json), "{\"csrf\":\"%s\"}", csrfToken);
    server.send(200, "application/json", json);
}

// Two different things a caller can mean by "the config", reported as two
// objects rather than one: `active` is what the radio booted with, `saved` is
// what is on the card and will apply next boot. Returning only `active` meant
// the settings form redisplayed boot values over an operator's unrebooted
// save, and posting that form silently reverted it (audit A08).
void handleConfigGet() {
    const ProfileOverrides active = radioActiveOverrides();
    ProfileOverrides saved;
    // An unreadable card is not evidence that nothing is saved, so the form
    // falls back to what is running rather than to hardcoded defaults.
    if (!readProfileConfigFromSD(saved)) saved = active;

    const ChannelParams amt = resolvedChannelForProfile(active, MissionProfile::MESHTASTIC);
    const ChannelParams amc = resolvedChannelForProfile(active, MissionProfile::MESHCORE);
    const ChannelParams smt = resolvedChannelForProfile(saved, MissionProfile::MESHTASTIC);
    const ChannelParams smc = resolvedChannelForProfile(saved, MissionProfile::MESHCORE);

    char json[640]; // two full preset pairs; worst case measures ~390B
    int n = snprintf(json, sizeof(json), "{\"active_profile\":\"%s\",",
                     missionProfileName((uint8_t)radioActiveProfile()));
    const struct { const char *key; const ChannelParams *mt; const ChannelParams *mc; } sets[] = {
        {"active", &amt, &amc}, {"saved", &smt, &smc},
    };
    for (size_t i = 0; i < 2 && n > 0 && (size_t)n < sizeof(json); i++) {
        n += snprintf(json + n, sizeof(json) - (size_t)n,
                      "\"%s\":{"
                      "\"meshtastic\":{\"freq_mhz\":%.3f,\"sf\":%u,\"bw_khz\":%.1f,\"cr_denom\":%u,\"sync_word\":%u},"
                      "\"meshcore\":{\"freq_mhz\":%.3f,\"sf\":%u,\"bw_khz\":%.1f,\"cr_denom\":%u,\"sync_word\":%u}}%s",
                      sets[i].key,
                      (double)sets[i].mt->freq_mhz, (unsigned)sets[i].mt->sf,
                      (double)sets[i].mt->bw_khz, (unsigned)sets[i].mt->cr_denom,
                      (unsigned)sets[i].mt->sync_word,
                      (double)sets[i].mc->freq_mhz, (unsigned)sets[i].mc->sf,
                      (double)sets[i].mc->bw_khz, (unsigned)sets[i].mc->cr_denom,
                      (unsigned)sets[i].mc->sync_word,
                      i == 0 ? "," : "}");
    }
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"config too large\"}");
        return;
    }
    server.send(200, "application/json", json);
}

// Saves ONE profile's preset — which one comes from the required `profile`
// field, not from whichever profile happens to be active right now. An
// earlier version captured whatever was currently active, so saving while
// on MeshCore silently wrote MeshCore's values into what the firmware
// would apply as a *Meshtastic* override on the next boot — profile label
// and radio config would disagree. Naming the target explicitly fixes it.
void handleConfigPost() {
    if (!stateChangeAllowed()) return;
    if (!server.hasArg("profile")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing profile\"}");
        return;
    }
    MissionProfile profile = MissionProfile::MESHTASTIC;
    if (!parseProfileArg(server.arg("profile"), profile)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"profile must be meshtastic or meshcore\"}");
        return;
    }

    ProfileOverrides current;
    // The persisted file, not the boot snapshot: a form that edits one field
    // must merge into what is already saved, not into what booted (A08).
    if (!readProfileConfigFromSD(current)) current = radioActiveOverrides();
    ChannelParams p = resolvedChannelForProfile(current, profile);

    // channel_config.h validates the whole token and the resulting value
    // before it is narrowed. String::toInt()/toFloat() mapped junk to 0 and
    // truncated 263 to SF7, and any positive number was an acceptable
    // bandwidth (A08). Browser form bounds are not server validation.
    static const char *FIELDS[] = {"freq_mhz", "sf", "bw_khz", "cr_denom", "sync_word"};
    for (const char *field : FIELDS) {
        if (!server.hasArg(field)) continue;
        if (!channelApplyValue(p, field, server.arg(field).c_str())) {
            char err[96];
            snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"invalid %s\"}", field);
            server.send(400, "application/json", err);
            return;
        }
    }

    if (!writeProfileConfigToSD(profile, p, current)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid values or SD unavailable\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// Display is deliberately a persisted preset, not a live UI-task mutation:
// the UI task owns the active dim timer/backlight state.  Matching channel
// preset behavior keeps that ownership intact and makes the reboot boundary
// clear to the operator.
void handleDisplayGet() {
    if (!loggerSdReady()) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"SD unavailable\"}");
        return;
    }

    DisplaySettings settings;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            server.send(503, "application/json", "{\"ok\":false,\"error\":\"SD busy\"}");
            return;
        }
        // This is a boot-time loader and deliberately takes no lock itself;
        // the panel holds the shared SPI lock across this bounded read.
        loadDisplaySettingsFromSD(settings);
    }

    char json[96];
    snprintf(json, sizeof(json),
             "{\"brightness_pct\":%u,\"idle_timeout_index\":%u}",
             (unsigned)settings.brightness_pct, (unsigned)settings.idle_timeout_index);
    server.send(200, "application/json", json);
}

void handleDisplayPost() {
    if (!stateChangeAllowed()) return;
    uint8_t brightness = 0;
    uint8_t idleIndex = 0;
    if (!parseUint8Arg("brightness_pct", 5, 100, brightness) || brightness % 5 != 0 ||
        !parseUint8Arg("idle_timeout_index", 0, 4, idleIndex)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid display values\"}");
        return;
    }

    DisplaySettings settings;
    settings.brightness_pct = brightness;
    settings.idle_timeout_index = idleIndex;
    if (!writeDisplaySettingsToSD(settings)) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"SD unavailable or busy\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// These are existing on-device menu toggles only: identity capture controls
// whether decoded node observations reach nodes.csv, and verbose debug
// changes Core 0 serial diagnostics.  Radio tuning/acquisition, AP control,
// SD recovery, and serial-access policy stay on the physical device.
void handleOptionsGet() {
    char json[80];
    snprintf(json, sizeof(json), "{\"identity_capture\":%s,\"verbose_debug\":%s}",
             radioIdentityCaptureIsEnabled() ? "true" : "false",
             loggerDebugIsEnabled() ? "true" : "false");
    server.send(200, "application/json", json);
}

void handleOptionsPost() {
    if (!stateChangeAllowed()) return;
    uint8_t identity = 0;
    uint8_t debug = 0;
    if (!parseUint8Arg("identity_capture", 0, 1, identity) ||
        !parseUint8Arg("verbose_debug", 0, 1, debug)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"options must be 0 or 1\"}");
        return;
    }

    radioIdentityCaptureSetEnabled(identity != 0);
    loggerDebugSetEnabled(debug != 0);
    server.send(200, "application/json", "{\"ok\":true}");
}

void registerRoutes() {
    // WebServer::stop() closes the listener but deliberately keeps its
    // RequestHandler list.  Registering again on every AP start therefore
    // leaks one duplicate handler allocation per route on every WiFi cycle.
    // The server object lives for the firmware lifetime, so one-time route
    // registration is both sufficient and the only bounded lifecycle.
    static bool routesRegistered = false;
    if (routesRegistered) return;

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/session", HTTP_GET, handleSession);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/runs", HTTP_GET, handleRuns);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.on("/api/display", HTTP_GET, handleDisplayGet);
    server.on("/api/display", HTTP_POST, handleDisplayPost);
    server.on("/api/options", HTTP_GET, handleOptionsGet);
    server.on("/api/options", HTTP_POST, handleOptionsPost);
    server.onNotFound(handleNotFound);
    // WebServer keeps only the headers it is told to keep.
    static const char *COLLECTED[] = {"Origin", "X-CSRF-Token"};
    server.collectHeaders(COLLECTED, sizeof(COLLECTED) / sizeof(COLLECTED[0]));
    routesRegistered = true;
}

void startAp() {
    memoryStatsLog("wifi-start-before");
    const char *ssid = ssidCached();

    csrfTokenRegenerate();
    // Per device, not per firmware build (audit A10). apKeyLoad() persists on
    // first use, so this is the same key across reboots unless the operator
    // deletes /loratrace/wifi.txt.
    char key[AP_KEY_BUF];
    apKeyPersisted = apKeyLoad(key, sizeof(key));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, key);
    registerRoutes();
    server.begin();
    apActive = true;
    // Survey rows record the conditions they were measured under, and a
    // 2.4GHz radio transmitting beside the receiver is one of them.
    radioNoteWifiActive(true);

    // One buffer, one print call, under the Serial lock — an earlier
    // unlocked version of this exact line printed with the SSID missing,
    // and torn again even after a buffer-only fix (see serial_lock.h).
    // The key is deliberately absent here. A serial console is the one place
    // a capture-session transcript is most likely to be shared, and the
    // operator reads the key off the device's own screen instead.
    char line[96];
    snprintf(line, sizeof(line), "[wifi] AP started: %s @ %s%s", ssid, WIFI_AP_IP,
             apKeyPersisted ? "" : " (key not saved to SD — it will change on reboot)");
    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) serialPrintln(line);
    }
    memoryStatsLog("wifi-start-after");
}

void stopAp() {
    memoryStatsLog("wifi-stop-before");
    server.stop();
    // Full teardown, not just "stop accepting connections" — the whole
    // point of on-demand is that the RAM/CPU/RF-noise cost actually goes
    // away when off, not just goes idle.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apActive = false;
    radioNoteWifiActive(false);
    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) serialPrintln("[wifi] AP stopped.");
    }
    memoryStatsLog("wifi-stop-after");
}

// Logs a connect/disconnect the moment the station count changes, instead
// of only being visible by eyeballing two /api/status polls. Polled here
// (edge-detected against the last-seen count) rather than via
// WiFi.onEvent(), since that callback runs outside this task's context and
// everything else here stays in this task's own loop.
uint8_t lastClientCount = 0;

void logClientCountChanges() {
    const uint8_t clients = (uint8_t)WiFi.softAPgetStationNum();
    if (clients == lastClientCount) return;
    char line[48];
    snprintf(line, sizeof(line), "[wifi] client %s, %u total",
             clients > lastClientCount ? "connected" : "disconnected", (unsigned)clients);
    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) serialPrintln(line);
    }
    lastClientCount = clients;
}

void wifiTask(void *) {
    memoryStatsRegisterCurrentTask(MemoryTask::WIFI);
    for (;;) {
        if (apRequested && !apActive) {
            startAp();
        } else if (!apRequested && apActive) {
            stopAp();
        }

        if (apActive) {
            logClientCountChanges();
            server.handleClient();
            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            // Nothing to do while off — lowest priority task, cost as
            // close to zero as possible. Reset so the next AP session
            // starts from a clean baseline.
            lastClientCount = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

} // namespace

bool wifiTaskStart() {
    BaseType_t ok = xTaskCreatePinnedToCore(wifiTask, "wifi", 8192, nullptr, 1, nullptr, 0);
    return ok == pdPASS;
}

void wifiRequestEnabled(bool enabled) {
    // One owned requested state. Read-then-toggle against the *actual* state
    // meant two ON commands issued before the AP finished starting cancelled
    // each other out, and an OFF during startup did nothing (audit A15).
    apRequested = enabled;
}

void wifiToggle() {
    apRequested = !apRequested;
}

bool wifiIsEnabled() {
    return apActive;
}

bool wifiIsRequested() {
    return apRequested;
}

bool wifiShutdownRequested() {
    return !apRequested && apActive;
}

uint8_t wifiClientCount() {
    return apActive ? (uint8_t)WiFi.softAPgetStationNum() : 0;
}

void wifiApSsid(char *buf, size_t bufLen) {
    if (buf == nullptr || bufLen == 0) return;
    strncpy(buf, ssidCached(), bufLen - 1);
    buf[bufLen - 1] = '\0';
}

void wifiApKey(char *buf, size_t bufLen) {
    if (buf == nullptr || bufLen == 0) return;
    // Loaded on first AP start; before that there is nothing to show yet.
    strncpy(buf, apKeyCurrent(), bufLen - 1);
    buf[bufLen - 1] = '\0';
}

bool wifiApKeyPersisted() {
    return apKeyPersisted;
}
