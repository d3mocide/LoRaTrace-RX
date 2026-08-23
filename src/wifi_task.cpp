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
#include "gps_task.h"
#include "logger_task.h"
#include "radio_task.h"
#include "run_log.h"
#include "spi_bus.h"
#include "web_assets.h"

namespace {

// WPA2-PSK, not open: this device is out in the field capturing other
// people's mesh traffic, and an open AP would let anyone nearby reconfigure
// the radio or pull your logs. A fixed default rather than SD-configurable
// for now — same "smallest thing that's actually useful" discipline the
// rest of Phase 2 followed; making this editable via config.txt is a cheap,
// natural follow-up, not done here to keep this change's scope to what was
// asked for.
constexpr const char *WIFI_AP_PASSWORD = "loratrace123";

WebServer server(80);
volatile bool apRequested = false;
bool apActive = false;

// Computed once and cached, rather than recomputed into a fresh stack
// buffer every time it's needed (as an earlier version of this file did) —
// a real hardware run showed the AP-started log line print with the SSID
// missing once, out of several successful boots that showed it correctly.
// No definitive root cause was found by inspection (ESP.getEfuseMac() is a
// deterministic hardware read, and nothing in startAp() should be able to
// touch a local buffer between filling it and printing it), but a single
// long-lived buffer, filled once, removes an entire category of doubt
// regardless — one write, many reads, nothing left to race or corrupt on
// a second AP start after a stop/start cycle.
char cachedSsid[32] = {0};

const char *ssidCached() {
    if (cachedSsid[0] == '\0') {
        // Chip-unique suffix (efuse MAC, always readable regardless of
        // WiFi state) so multiple LoRaTrace units nearby don't collide on
        // the same SSID — a real scenario for this project specifically,
        // since a wardrive is exactly the kind of activity multiple people
        // might do together.
        const uint64_t mac = ESP.getEfuseMac();
        snprintf(cachedSsid, sizeof(cachedSsid), "LoRaTrace-%04X", (unsigned)(mac & 0xFFFFu));
    }
    return cachedSsid;
}

// How long a handler will wait for the shared SPI bus. Bounded, not
// portMAX_DELAY, matching every other non-radio SD caller in this codebase
// (logger_task.cpp's own BUS_WAIT) — an HTTP request is not worth stalling
// indefinitely for, and the client just gets a 503 to retry.
constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(2000);

// Bytes per chunk when streaming a CSV off SD. Small enough that each lock
// acquisition is brief (this is the one thing that must never make the
// radio task wait), large enough not to spend all its time on per-chunk
// open/seek/close overhead.
constexpr size_t CSV_CHUNK_SIZE = 512;

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
    GpsFix fix = {};
    const bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(100));
    const bool positioned = haveFix && fix.has_position;

    char json[512];
    const int n = snprintf(
        json, sizeof(json),
        "{"
        "\"rx\":%lu,\"crc_err\":%lu,\"queue_drop\":%lu,\"bus_miss\":%lu,"
        "\"rows\":%lu,\"row_drop\":%lu,\"flushes\":%lu,\"max_flush_ms\":%lu,\"max_session_ms\":%lu,"
        "\"sd_ready\":%s,\"session_rows\":%lu,\"run\":%u,"
        "\"nmea\":%lu,\"nmea_bad_crc\":%lu,"
        "\"has_fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"sats\":%u,\"sats_in_view\":%u,"
        "\"heap_free\":%lu,\"heap_min\":%lu,\"batt_mv\":%lu,\"wifi_clients\":%u"
        "}",
        (unsigned long)radioPacketCount(), (unsigned long)radioCrcErrorCount(),
        (unsigned long)radioQueueDropCount(), (unsigned long)radioBusMissCount(),
        (unsigned long)loggerRowsWritten(), (unsigned long)loggerRowsDropped(),
        (unsigned long)loggerFlushCount(), (unsigned long)loggerMaxFlushMs(),
        (unsigned long)loggerMaxSessionMs(), loggerSdReady() ? "true" : "false",
        (unsigned long)loggerSessionRows(), (unsigned)loggerRunIndex(),
        (unsigned long)gpsSentenceCount(), (unsigned long)gpsChecksumErrorCount(),
        positioned ? "true" : "false", positioned ? fix.lat : 0.0, positioned ? fix.lon : 0.0,
        (unsigned)fix.satellites, (unsigned)fix.sats_in_view, (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(), (unsigned long)batteryMilliVolts(), (unsigned)wifiClientCount());

    if (n < 0 || (size_t)n >= sizeof(json)) {
        server.send(500, "text/plain", "status too large");
        return;
    }
    server.send(200, "application/json", json);
}

// Walks /loratrace for runNNNN directories, same shape as
// logger_task.cpp's own highestRunIndexLocked() but collecting every valid
// index rather than just the max. Kept as wifi_task's own copy rather than
// exported from logger_task.h — this needs its own SpiBusLock regardless,
// and not sharing mutable file-scope state across translation units is
// worth a dozen duplicated lines given how much logger_task.cpp is
// load-bearing for the Phase 2 exit criterion.
void handleRuns() {
    String json = "[";
    bool first = true;

    SpiBusLock lock(BUS_WAIT);
    if (lock.held()) {
        File dir = SD.open(CHANNEL_CONFIG_DIR);
        if (dir) {
            for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
                const uint16_t idx = runIndexFromName(entry.name());
                entry.close();
                if (idx == 0) continue;
                if (!first) json += ',';
                json += String(idx);
                first = false;
            }
            dir.close();
        }
    }
    json += ']';
    server.send(200, "application/json", json);
}

// Streams `path` (a detections.csv or session.csv inside a run directory)
// as a chunked download. The one place correctness really matters: the SPI
// bus lock is acquired fresh for each chunk and released before the slow
// part (writing to the TCP socket), exactly mirroring
// logger_task.cpp's appendToFile() discipline — never hold the bus, or the
// SD file, across anything that isn't a single bounded SD operation. A
// multi-hour run's CSV can be large; holding the lock for the whole
// transfer would stall the radio task, which is the one thing this entire
// feature must never do.
void streamCsvFile(const char *path, const char *downloadName) {
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

    server.sendHeader("Content-Disposition", String("attachment; filename=\"") + downloadName + "\"");
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
        server.client().write(buf, (size_t)readLen);
        offset += (size_t)readLen;
    }
}

// Matches "/api/runs/<n>/detections.csv" or ".../session.csv". Hand-parsed
// rather than relying on WebServer's path-pattern support — that's a
// version-specific feature this codebase's pinned core (2.0.17, see
// platformio.ini) shouldn't be assumed to have, and this is a small, fixed
// shape anyway (same "hand-roll it, it's simple and known" call as
// detection.h's own CSV formatting).
void handleNotFound() {
    const String uri = server.uri();
    if (uri.startsWith("/api/runs/")) {
        int idx = 0;
        char leaf[32] = {0};
        if (sscanf(uri.c_str(), "/api/runs/%d/%31s", &idx, leaf) == 2 && idx > 0 &&
            (strcmp(leaf, "detections.csv") == 0 || strcmp(leaf, "session.csv") == 0)) {
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

void handleConfigGet() {
    const ChannelParams p = radioActiveChannel();
    char json[160];
    snprintf(json, sizeof(json),
             "{\"freq_mhz\":%.3f,\"sf\":%u,\"bw_khz\":%.1f,\"cr_denom\":%u,\"sync_word\":%u}",
             (double)p.freq_mhz, (unsigned)p.sf, (double)p.bw_khz, (unsigned)p.cr_denom,
             (unsigned)p.sync_word);
    server.send(200, "application/json", json);
}

void handleConfigPost() {
    // Start from the currently active values so a form that only edits one
    // field doesn't zero the rest.
    ChannelParams p = radioActiveChannel();
    if (server.hasArg("freq_mhz")) p.freq_mhz = server.arg("freq_mhz").toFloat();
    if (server.hasArg("sf")) p.sf = (uint8_t)server.arg("sf").toInt();
    if (server.hasArg("bw_khz")) p.bw_khz = server.arg("bw_khz").toFloat();
    if (server.hasArg("cr_denom")) p.cr_denom = (uint8_t)server.arg("cr_denom").toInt();
    if (server.hasArg("sync_word")) {
        p.sync_word = (uint8_t)strtol(server.arg("sync_word").c_str(), nullptr, 0);
    }

    if (!writeChannelConfigToSD(p)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid values or SD unavailable\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

void registerRoutes() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/runs", HTTP_GET, handleRuns);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.onNotFound(handleNotFound);
}

void startAp() {
    const char *ssid = ssidCached();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, WIFI_AP_PASSWORD);
    registerRoutes();
    server.begin();
    apActive = true;

    // One buffer, one print call — not four separate ones. A hardware run
    // showed this exact line print with the SSID silently missing once;
    // see PROGRESS.md and main.cpp's loop() for the full read (unsynchronized
    // Serial access across cores/tasks loses whole pieces of a multi-call
    // sequence when another task's print lands in the middle of it).
    char line[64];
    snprintf(line, sizeof(line), "[wifi] AP started: %s @ %s", ssid, WIFI_AP_IP);
    Serial.println(line);
}

void stopAp() {
    server.stop();
    // Full teardown, not just "stop accepting connections" — the whole
    // point of on-demand is that the RAM/CPU/RF-noise cost actually goes
    // away when off, not just goes idle.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apActive = false;
    Serial.println(F("[wifi] AP stopped."));
}

// Logs a connect/disconnect the moment the station count changes, rather
// than leaving it as something only visible by comparing two /api/status
// polls (or the WIFI page) by eye. Polled here (edge-detected against the
// last-seen count) rather than via WiFi.onEvent(): that callback runs
// outside this task's context, and every other piece of state in this file
// is deliberately kept to this one task's own loop — one less cross-context
// question to answer for a feature this small. At a 2ms loop interval while
// the AP is active, an actual connect/disconnect is caught well within
// human-noticeable time.
uint8_t lastClientCount = 0;

void logClientCountChanges() {
    const uint8_t clients = (uint8_t)WiFi.softAPgetStationNum();
    if (clients == lastClientCount) return;
    char line[48];
    snprintf(line, sizeof(line), "[wifi] client %s, %u total",
             clients > lastClientCount ? "connected" : "disconnected", (unsigned)clients);
    Serial.println(line);
    lastClientCount = clients;
}

void wifiTask(void *) {
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
            // Nothing to do while off — this task is the lowest priority in
            // the system and should cost as close to zero as possible then.
            // Reset so the next AP session starts from a clean baseline
            // rather than comparing against whatever the count was when
            // this one was toggled off.
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
