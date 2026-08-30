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

// WPA2-PSK, not open: this device is out in the field capturing other
// people's mesh traffic, and an open AP would let anyone nearby reconfigure
// the radio or pull your logs. Fixed default rather than SD-configurable
// for now (see SECURITY.md) — a cheap, natural follow-up, not in scope here.
constexpr const char *WIFI_AP_PASSWORD = "loratrace123";

WebServer server(80);
volatile bool apRequested = false;
bool apActive = false;

// Computed once and cached rather than recomputed on every use — a real
// hardware run showed the AP-started log line print with the SSID missing
// once. No definitive root cause found (ESP.getEfuseMac() is deterministic
// and nothing should touch a local buffer between fill and print), but one
// long-lived buffer, filled once, removes the whole category of doubt.
char cachedSsid[32] = {0};

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
    while (offset < fileSize) {
        // File::read() returns int and can be negative on error — read into
        // a signed local first. Assigning a -1 error straight into a size_t
        // would turn "read failed" into "read 4 billion bytes" and send
        // garbage from `buf` under a bogus huge length.
        int readLen = 0;
        {
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) break; // bus busy — client gets a short/incomplete file, not a stall
            File f = SD.open(path); // read mode, same default as above
            if (!f) break;
            f.seek(offset);
            readLen = f.read(buf, sizeof(buf));
            f.close();
        }
        if (readLen <= 0) break;
        WiFiClient client = server.client();
        if (!client.connected()) break;
        const size_t written = client.write(buf, (size_t)readLen);
        if (written != (size_t)readLen) break;
        offset += written;

        // WiFiClient::write() can spend multiple seconds retrying a full TCP
        // socket. Yield between successful chunks so the Core 0 idle task can
        // service the watchdog during a large SD download.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    memoryStatsLog("csv-download-after");
}

// Matches "/api/runs/<n>/<known.csv>". Hand-parsed
// rather than relying on WebServer's path-pattern support, since that's a
// version-specific feature this codebase's pinned core (2.0.17) shouldn't
// be assumed to have — and the shape here is small and fixed anyway.
void handleNotFound() {
    const String uri = server.uri();
    if (uri.startsWith("/api/runs/")) {
        int idx = 0;
        char leaf[32] = {0};
        if (sscanf(uri.c_str(), "/api/runs/%d/%31s", &idx, leaf) == 2 && idx > 0 &&
            (strcmp(leaf, "detections.csv") == 0 || strcmp(leaf, "session.csv") == 0 ||
             strcmp(leaf, "probe.csv") == 0 || strcmp(leaf, "energy.csv") == 0 ||
             strcmp(leaf, "nodes.csv") == 0)) {
            char path[RUN_PATH_MAX];
            if (runFilePath(path, sizeof(path), CHANNEL_CONFIG_DIR, (uint16_t)idx, leaf) > 0) {
                char downloadName[48];
                snprintf(downloadName, sizeof(downloadName), "run%04u_%s", (unsigned)idx, leaf);
                streamCsvFile(path, downloadName);
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

// Both profiles' currently-resolved values (override if loaded, else
// hardcoded default), plus which one HOME_LISTEN is actually locked to.
// Reads radioActiveOverrides()/radioActiveProfile() rather than re-parsing
// config.txt — a save through handleConfigPost() below only updates them
// on the next boot, same "not live" boundary the channel override has.
void handleConfigGet() {
    const ProfileOverrides ov = radioActiveOverrides();
    const ChannelParams mt = resolvedChannelForProfile(ov, MissionProfile::MESHTASTIC);
    const ChannelParams mc = resolvedChannelForProfile(ov, MissionProfile::MESHCORE);
    char json[320]; // worst case (3-digit freq/sf/bw/sync on both profiles) measures ~201B — real margin
    snprintf(json, sizeof(json),
             "{\"active_profile\":\"%s\","
             "\"meshtastic\":{\"freq_mhz\":%.3f,\"sf\":%u,\"bw_khz\":%.1f,\"cr_denom\":%u,\"sync_word\":%u},"
             "\"meshcore\":{\"freq_mhz\":%.3f,\"sf\":%u,\"bw_khz\":%.1f,\"cr_denom\":%u,\"sync_word\":%u}}",
             missionProfileName((uint8_t)radioActiveProfile()), (double)mt.freq_mhz, (unsigned)mt.sf,
             (double)mt.bw_khz, (unsigned)mt.cr_denom, (unsigned)mt.sync_word, (double)mc.freq_mhz,
             (unsigned)mc.sf, (double)mc.bw_khz, (unsigned)mc.cr_denom, (unsigned)mc.sync_word);
    server.send(200, "application/json", json);
}

// Saves ONE profile's preset — which one comes from the required `profile`
// field, not from whichever profile happens to be active right now. An
// earlier version captured whatever was currently active, so saving while
// on MeshCore silently wrote MeshCore's values into what the firmware
// would apply as a *Meshtastic* override on the next boot — profile label
// and radio config would disagree. Naming the target explicitly fixes it.
void handleConfigPost() {
    if (!server.hasArg("profile")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing profile\"}");
        return;
    }
    MissionProfile profile;
    if (!parseProfileArg(server.arg("profile"), profile)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"profile must be meshtastic or meshcore\"}");
        return;
    }

    const ProfileOverrides current = radioActiveOverrides();
    // Start from this profile's own currently-resolved values so a form
    // that only edits one field doesn't zero the rest.
    ChannelParams p = resolvedChannelForProfile(current, profile);
    if (server.hasArg("freq_mhz")) p.freq_mhz = server.arg("freq_mhz").toFloat();
    if (server.hasArg("sf")) p.sf = (uint8_t)server.arg("sf").toInt();
    if (server.hasArg("bw_khz")) p.bw_khz = server.arg("bw_khz").toFloat();
    if (server.hasArg("cr_denom")) p.cr_denom = (uint8_t)server.arg("cr_denom").toInt();
    if (server.hasArg("sync_word")) {
        p.sync_word = (uint8_t)strtol(server.arg("sync_word").c_str(), nullptr, 0);
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
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/runs", HTTP_GET, handleRuns);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.on("/api/display", HTTP_GET, handleDisplayGet);
    server.on("/api/display", HTTP_POST, handleDisplayPost);
    server.on("/api/options", HTTP_GET, handleOptionsGet);
    server.on("/api/options", HTTP_POST, handleOptionsPost);
    server.onNotFound(handleNotFound);
    routesRegistered = true;
}

void startAp() {
    memoryStatsLog("wifi-start-before");
    const char *ssid = ssidCached();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, WIFI_AP_PASSWORD);
    registerRoutes();
    server.begin();
    apActive = true;

    // One buffer, one print call, under the Serial lock — an earlier
    // unlocked version of this exact line printed with the SSID missing,
    // and torn again even after a buffer-only fix (see serial_lock.h).
    char line[64];
    snprintf(line, sizeof(line), "[wifi] AP started: %s @ %s", ssid, WIFI_AP_IP);
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

void wifiToggle() {
    apRequested = !apRequested;
}

bool wifiIsEnabled() {
    return apActive;
}

uint8_t wifiClientCount() {
    return apActive ? (uint8_t)WiFi.softAPgetStationNum() : 0;
}

void wifiApSsid(char *buf, size_t bufLen) {
    if (buf == nullptr || bufLen == 0) return;
    strncpy(buf, ssidCached(), bufLen - 1);
    buf[bufLen - 1] = '\0';
}
