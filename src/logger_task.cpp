#include "logger_task.h"

#include <Arduino.h>
#include <SD.h>
#include <freertos/task.h>

#include "battery.h"
#include "board_pins.h"
#include "detection.h"
#include "gps_task.h"
#include "radio_task.h"
#include "run_log.h"
#include "session_log.h"
#include "spi_bus.h"

namespace {

constexpr const char *LOG_DIR = "/loratrace";
// Leaf names inside this run's directory. One wardrive is one folder
// (run_log.h): /loratrace/runNNNN/{detections,session}.csv.
constexpr const char *DETECTIONS_LEAF = "detections.csv";
constexpr const char *SESSION_LEAF = "session.csv";

// Resolved once, on the first successful mount of this power-on.
uint16_t runIndex = 0;
char detectionsPath[RUN_PATH_MAX];
char sessionPath[RUN_PATH_MAX];

// ~2KB holds roughly 15-20 rows. Sized to keep a single flush short (see
// the header): bigger buffers mean longer bus holds, which is exactly the
// thing that costs packets.
constexpr size_t BATCH_BUF_SIZE = 2048;
// Flush when the buffer is this full, leaving room for one more max-length
// row so a row is never split across flushes.
constexpr size_t BATCH_HIGH_WATER = BATCH_BUF_SIZE - 192;
// Flush at least this often even when quiet, so a log is never more than a
// few seconds behind reality if the device is unplugged.
constexpr uint32_t FLUSH_INTERVAL_MS = 5000;
// A GPS fix older than this is treated as unusable rather than attributed
// to a new detection. At driving speed 10s is already ~250m of error;
// beyond that an empty coordinate is more honest than a stale one.
constexpr uint32_t FIX_MAX_AGE_MS = 10000;
// Health-row cadence. Slow on purpose: this is a trend record, not a
// monitor. One row a minute is ~180 over a three-hour drive — invisible
// next to the detection log, and one extra short bus hold per minute is
// far below the noise floor of the flushes already happening.
constexpr uint32_t SESSION_INTERVAL_MS = 60000;

constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(2000);

QueueHandle_t detectionQueue = nullptr;
bool sdReady = false;

// Menu-toggled (ui_task.cpp); both it and this task run on Core 0, so a
// plain bool is enough — no cross-core visibility concern the way a Core
// 1/Core 0 flag would have.
volatile bool debugVerbose = false;

char batchBuf[BATCH_BUF_SIZE];
size_t batchLen = 0;
uint32_t batchRows = 0; // rows currently buffered but not yet on the card

volatile uint32_t rowsWritten = 0;
volatile uint32_t rowsDropped = 0;
volatile uint32_t flushCount = 0;
volatile uint32_t maxFlushMs = 0;
volatile uint32_t sessionRows = 0;
volatile uint32_t maxSessionMs = 0;

// Highest runNNNN index already on the card, or 0 if there are none.
// Assumes the caller holds the bus and SD is mounted.
//
// Scans rather than trusting a stored counter: the listing is the truth, it
// can't drift, and there's no mutable state to corrupt on a power cut. If
// the directory won't open at all, fall back to probing upward — bounded,
// because an unbounded probe on a sick card would hang the logger before it
// ever wrote a row.
uint16_t highestRunIndexLocked() {
    uint16_t highest = 0;

    File dir = SD.open(LOG_DIR);
    if (dir) {
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            const uint16_t index = runIndexFromName(entry.name());
            if (index > highest) highest = index;
            entry.close();
        }
        dir.close();
        return highest;
    }

    char probe[RUN_PATH_MAX];
    for (uint16_t i = 1; i <= 512; i++) {
        if (runDirPath(probe, sizeof(probe), LOG_DIR, i) == 0) break;
        if (!SD.exists(probe)) return (uint16_t)(i - 1);
    }
    return highest;
}

// Creates `path` with `header` as its first line if it doesn't exist yet.
// Assumes the caller holds the bus and SD is mounted.
bool ensureCsvLocked(const char *path, const char *header) {
    if (SD.exists(path)) return true;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.println(header);
    f.close();
    return true;
}

// Mounts SD and ensures both CSVs exist with their header rows. Assumes the
// caller holds the bus.
bool openLogsLocked() {
    // SD.end() first, always. The ESP32 Arduino core's SD.begin() returns
    // true immediately when a card object already exists
    // (`if(_card) return true;` in SD.cpp), so once a card has been mounted
    // and then pulled, the retry path below would "succeed" forever while
    // every single write kept failing — the exact mid-session reseat this
    // firmware is supposed to survive, silently not surviving. Tearing the
    // mount down first makes the remount real. It also makes this the sole
    // authority on the mount regardless of what the boot-time config read
    // (config.cpp) left behind.
    SD.end();
    if (!SD.begin(PIN_SD_CS, sharedSpi())) return false;
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);

    // Resolve the run once per power-on, not once per mount. A card pulled
    // and reseated mid-drive rejoins the run it left rather than splitting
    // one drive across two folders — the gap shows up as `sd` going down
    // and back in this run's own health rows, which is the honest record of
    // what happened.
    if (runIndex == 0) {
        runIndex = runNextIndex(highestRunIndexLocked());

        char runDir[RUN_PATH_MAX];
        if (runDirPath(runDir, sizeof(runDir), LOG_DIR, runIndex) == 0) return false;
        if (!SD.exists(runDir) && !SD.mkdir(runDir)) return false;

        if (runFilePath(detectionsPath, sizeof(detectionsPath), LOG_DIR, runIndex,
                        DETECTIONS_LEAF) == 0) {
            return false;
        }
        if (runFilePath(sessionPath, sizeof(sessionPath), LOG_DIR, runIndex, SESSION_LEAF) == 0) {
            return false;
        }
    }

    if (!ensureCsvLocked(detectionsPath, LOG_CSV_HEADER)) return false;
    // A missing health log must not stop detections being logged: the
    // detections are the mission, this is instrumentation.
    ensureCsvLocked(sessionPath, SESSION_CSV_HEADER);
    return true;
}

// The two failure modes need different handling by the caller, so they are
// distinguished rather than collapsed into a bool: a busy bus means "try
// again shortly", a file error means "the card is gone".
enum class WriteResult { OK, BUS_BUSY, FILE_ERROR };

// Appends `len` bytes to `path` in exactly one open/write/close, acquiring
// the bus itself and holding it for nothing more. The caller must NOT
// already hold the bus.
//
// `worstMs` is the caller's own high-water mark, tracked PER WRITER rather
// than shared. First hardware run of this code made the reason obvious: the
// status line read `flushes=0 maxflush=26ms`, because the boot health row
// had taken 26ms and charged it to the detection-flush metric. Both numbers
// answer real questions and they are different questions — "is the logger
// starving the radio?" is the max across both, while "is my batch sizing
// wrong?" is only ever about detection flushes. Merging them answered the
// first and quietly destroyed the second.
WriteResult appendToFile(const char *path, const char *data, size_t len,
                         volatile uint32_t &worstMs) {
    const uint32_t started = millis();
    WriteResult result;
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) return WriteResult::BUS_BUSY;

        File f = SD.open(path, FILE_APPEND);
        if (!f) {
            result = WriteResult::FILE_ERROR;
        } else {
            f.write((const uint8_t *)data, len);
            f.close();
            result = WriteResult::OK;
        }
    }
    const uint32_t elapsed = millis() - started;
    if (elapsed > worstMs) worstMs = elapsed;
    return result;
}

// Appends the batch buffer to SD.
void flushBatch() {
    if (batchLen == 0) return;

    switch (appendToFile(detectionsPath, batchBuf, batchLen, maxFlushMs)) {
        case WriteResult::BUS_BUSY:
            // Keep the data buffered and try again on the next pass rather
            // than discarding it — unlike the radio task, the logger is
            // allowed to be late, just not lossy.
            return;
        case WriteResult::FILE_ERROR:
            // The card went away mid-session. Drop this batch and fall back
            // to the retry path; buffering indefinitely would just consume
            // RAM on a device whose datastore is gone.
            sdReady = false;
            rowsDropped += batchRows;
            break;
        case WriteResult::OK:
            flushCount++;
            // Counted here, not at append time: "written" should mean it
            // reached the card, otherwise the diagnostic overstates what
            // survived a power cut.
            rowsWritten += batchRows;
            break;
    }
    batchLen = 0;
    batchRows = 0;
}

// Samples every counter this firmware exposes and appends one health row.
// Best-effort by design: a lost health row is worth exactly zero retries,
// and must never cost the detection log a flush.
void writeSessionRow(const char *reason) {
    if (!sdReady) return;

    GpsFix fix;
    const bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(50));
    const uint32_t now = millis();

    SessionStats s;
    s.run = runIndex;
    s.reason = reason;
    s.uptime_s = now / 1000;

    s.has_fix = haveFix && gpsFixIsFresh(fix, now, FIX_MAX_AGE_MS);
    s.lat = fix.lat;
    s.lon = fix.lon;
    s.sats = fix.satellites;
    s.sats_in_view = fix.sats_in_view;
    s.fix_type = fix.fix_type;
    const uint32_t firstFix = gpsFirstFixMillis();
    s.ttff_s = firstFix == 0 ? 0 : firstFix / 1000;
    s.nmea_sentences = gpsSentenceCount();
    s.nmea_bad_crc = gpsChecksumErrorCount();
    s.gps_max_loop_gap_ms = gpsMaxLoopGapMs();
    s.gps_oversize_drops = gpsOversizeDropCount();

    s.rx = radioPacketCount();
    s.crc_errors = radioCrcErrorCount();
    s.queue_drops = radioQueueDropCount();
    s.bus_misses = radioBusMissCount();

    s.rows_written = rowsWritten;
    s.rows_dropped = rowsDropped;
    s.flushes = flushCount;
    s.max_flush_ms = maxFlushMs;
    s.max_session_ms = maxSessionMs;
    s.sd_ready = sdReady;
    s.bus_contention = spiBusContentionCount();

    s.heap_free = ESP.getFreeHeap();
    s.heap_min = ESP.getMinFreeHeap();
    s.batt_mv = batteryMilliVolts();
    // nullptr = this task. Returns the high-water mark in words.
    s.logger_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);

    char timestamp[24];
    detectionFormatTimestamp(timestamp, sizeof(timestamp), haveFix && fix.has_time, fix.year,
                             fix.month, fix.day, fix.hour, fix.minute, fix.second);

    char row[320];
    size_t n = sessionFormatCsv(s, row, sizeof(row), timestamp);
    if (n == 0) return; // truncated — a malformed health row helps nobody
    row[n++] = '\n';   // snprintf guarantees n <= sizeof(row)-1, so this fits

    if (appendToFile(sessionPath, row, n, maxSessionMs) == WriteResult::OK) sessionRows++;
}

void appendDetection(const Detection &det) {
    GpsFix fix;
    bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(50));

    const uint32_t now = millis();
    const bool fresh = haveFix && gpsFixIsFresh(fix, now, FIX_MAX_AGE_MS);

    char timestamp[24];
    detectionFormatTimestamp(timestamp, sizeof(timestamp), haveFix && fix.has_time, fix.year,
                             fix.month, fix.day, fix.hour, fix.minute, fix.second);

    char row[256];
    size_t n = detectionFormatCsv(det, row, sizeof(row), timestamp, fresh, fix.lat, fix.lon,
                                  haveFix ? fix.fix_quality : 0, runIndex);
    if (n == 0) {
        rowsDropped++;
        return;
    }

    // Verbose debug mode (ui_task.cpp menu toggle): reuses the exact CSV
    // row already built above rather than a second format call — same
    // fields (profile/classification/RSSI/SNR/SF/BW/hop info), so this
    // never drifts from what actually lands on SD. Printed unconditionally
    // of `sdReady` below, on purpose: this exists specifically for bench
    // sessions that want to see live RX detail without an SD card or the
    // WiFi AP in the loop at all.
    //
    // ONE Serial.write() call, not a "[debug] " print + a row write + a
    // println — bench-caught the same session this shipped: main.cpp's
    // [status] line runs on Core 1 while this runs on Core 0, nothing
    // serializes Serial access between them, and a multi-call sequence
    // torn by another task's print landing mid-sequence is the exact
    // failure main.cpp's own [status] line comment already documents (and
    // PROGRESS.md's [wifi] SSID bug hit for real). A single write() is far
    // more likely to be atomic than N calls with gaps another task can
    // land in.
    if (debugVerbose) {
        char debugLine[8 + sizeof(row) + 1]; // "[debug] " + row + '\n'
        memcpy(debugLine, "[debug] ", 8);
        memcpy(debugLine + 8, row, n);
        debugLine[8 + n] = '\n';
        Serial.write((const uint8_t *)debugLine, 8 + n + 1);
    }

    if (!sdReady) {
        rowsDropped++;
        return;
    }

    // Flush first if this row wouldn't fit, so rows are never split.
    if (batchLen + n + 1 >= BATCH_BUF_SIZE) flushBatch();
    if (batchLen + n + 1 >= BATCH_BUF_SIZE) {
        rowsDropped++; // still no room (flush failed) — drop rather than corrupt
        return;
    }

    memcpy(batchBuf + batchLen, row, n);
    batchLen += n;
    batchBuf[batchLen++] = '\n';
    batchRows++;
}

void loggerTask(void *) {
    {
        SpiBusLock lock(portMAX_DELAY);
        if (lock.held()) sdReady = openLogsLocked();
    }

    uint32_t lastFlush = millis();
    uint32_t lastSession = millis();
    bool bootRowWritten = false;

    for (;;) {
        Detection det;
        // Wake either on a detection or on the flush interval, whichever
        // comes first — so a quiet period still commits buffered rows.
        if (xQueueReceive(detectionQueue, &det, pdMS_TO_TICKS(500)) == pdTRUE) {
            appendDetection(det);
        }

        const uint32_t now = millis();
        if (batchLen >= BATCH_HIGH_WATER || (batchLen > 0 && now - lastFlush >= FLUSH_INTERVAL_MS)) {
            flushBatch();
            lastFlush = now;
        }

        // Periodically retry a failed mount so a card inserted (or reseated)
        // mid-session starts working without a reboot. SD-mount reliability
        // is a known watch item on this board — PROGRESS.md.
        if (!sdReady && now - lastFlush >= FLUSH_INTERVAL_MS) {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) sdReady = openLogsLocked();
            lastFlush = now;
        }

        // Health row last in the loop body, so it samples counters that
        // already include this pass's work. The boot row goes out as soon as
        // the card is first available rather than a minute in: if a run dies
        // early, the one row that exists should still say when it started.
        // That also covers a card inserted after boot — the row then marks
        // the moment logging actually began, which is the useful timestamp.
        // A later remount does not repeat it; `sd` flipping down and back in
        // the periodic rows is what a mid-drive reseat looks like.
        if (sdReady && !bootRowWritten) {
            writeSessionRow("boot");
            bootRowWritten = true;
            lastSession = now;
        } else if (sdReady && now - lastSession >= SESSION_INTERVAL_MS) {
            writeSessionRow("periodic");
            lastSession = now;
        }
    }
}

} // namespace

bool loggerTaskStart(QueueHandle_t queue) {
    detectionQueue = queue;
    // Core 0, priority 2: above GPS (1) so log rows drain promptly, below
    // the radio (3) which must always win.
    //
    // 5120 rather than 4096: the health-row path (a GpsFix, a SessionStats
    // and a 320-byte row buffer) is a deeper frame than the detection path,
    // and it calls into SD/FatFS from the bottom of it. The margin is cheap
    // on a part with ~330KB of heap free, and the run reports back through
    // logger_stack_free in session.csv rather than leaving this a guess.
    BaseType_t ok = xTaskCreatePinnedToCore(loggerTask, "logger", 5120, nullptr, 2, nullptr, 0);
    return ok == pdPASS;
}

bool loggerSdReady() {
    return sdReady;
}
uint32_t loggerRowsWritten() {
    return rowsWritten;
}
uint32_t loggerRowsDropped() {
    return rowsDropped;
}
uint32_t loggerFlushCount() {
    return flushCount;
}
uint32_t loggerMaxFlushMs() {
    return maxFlushMs;
}
uint32_t loggerSessionRows() {
    return sessionRows;
}
uint16_t loggerRunIndex() {
    return runIndex;
}
uint32_t loggerMaxSessionMs() {
    return maxSessionMs;
}

void loggerDebugToggle() {
    debugVerbose = !debugVerbose;
    if (debugVerbose) {
        // Column header once on enable, so the CSV-shaped lines that follow
        // are actually readable rather than a wall of unlabeled commas. One
        // buffer, one Serial call — see the per-detection print above for
        // why two separate calls isn't safe here (bench-caught: this exact
        // line came out torn — "[debug] verbose " with "mode ON" missing —
        // on the same hardware pass that shipped debug mode).
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[debug] verbose mode ON\n[debug] %s\n", LOG_CSV_HEADER);
        if (n > 0) Serial.write((const uint8_t *)buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    } else {
        Serial.println(F("[debug] verbose mode OFF"));
    }
}
bool loggerDebugIsEnabled() {
    return debugVerbose;
}
