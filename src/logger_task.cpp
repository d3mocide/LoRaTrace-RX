#include "logger_task.h"

#include <Arduino.h>
#include <SD.h>
#include <freertos/task.h>

#include "battery.h"
#include "board_pins.h"
#include "detection.h"
#include "energy_observation.h"
#include "gps_task.h"
#include "memory_stats.h"
#include "serial_control.h"
#include "radio_task.h"
#include "run_log.h"
#include "serial_lock.h"
#include "session_log.h"
#include "spi_bus.h"

namespace {

constexpr const char *LOG_DIR = "/loratrace";
// Leaf names inside this run's directory. One wardrive is one folder
// (run_log.h): /loratrace/runNNNN/{detections,session}.csv.
constexpr const char *DETECTIONS_LEAF = "detections.csv";
constexpr const char *SESSION_LEAF = "session.csv";
constexpr const char *PROBE_LEAF = "probe.csv";
constexpr const char *ENERGY_LEAF = "energy.csv";
constexpr const char *NODES_LEAF = "nodes.csv";

// Resolved once, on the first successful mount of this power-on.
uint16_t runIndex = 0;
char detectionsPath[RUN_PATH_MAX];
char sessionPath[RUN_PATH_MAX];
char probePath[RUN_PATH_MAX];
char energyPath[RUN_PATH_MAX];
char nodesPath[RUN_PATH_MAX];

// ~2KB holds a few complete raw-frame rows. Sized to keep a single flush
// short (see the header): bigger buffers mean longer bus holds, which is
// exactly the thing that costs packets.
constexpr size_t BATCH_BUF_SIZE = 2048;
// Flush when the buffer is this full, leaving room for one more max-length
// row so a row is never split across flushes.
constexpr size_t BATCH_HIGH_WATER = BATCH_BUF_SIZE - DETECTION_CSV_MAX_ROW;
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
QueueHandle_t scanObservationQueue = nullptr;
QueueHandle_t energyObservationQueue = nullptr;
QueueHandle_t identityQueue = nullptr;
bool sdReady = false;
bool initialSdMounted = false;
volatile bool sdRetryRequested = false;
volatile bool loggerStarted = false;

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
volatile uint32_t maxScanMs = 0;
volatile uint32_t maxIdentityMs = 0;
volatile uint32_t scanRowsWritten = 0;
volatile uint32_t scanRowsDropped = 0;
volatile uint32_t maxEnergyMs = 0;
volatile uint32_t energyRowsWritten = 0;
volatile uint32_t energyRowsDropped = 0;
volatile uint32_t identityRowsWritten = 0;
volatile uint32_t identityRowsDropped = 0;

// Highest runNNNN index already on the card, or 0 if there are none.
// Assumes the caller holds the bus and SD is mounted.
//
// Scans rather than trusting a stored counter: the listing is the truth
// and there's no mutable state to corrupt on a power cut. Falls back to a
// bounded upward probe if the directory won't open, so a sick card can't
// hang the logger before it writes a row.
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

// Ensures all run CSVs exist with their header rows. When `remount` is true,
// first establishes a fresh SD mount; otherwise it intentionally adopts the
// successful mount left by the boot-time config read. Assumes the caller
// holds the bus.
bool openLogsLocked(bool remount) {
    if (remount) {
        // Arduino-ESP32's SD.begin() can report success while retaining a
        // stale card object after removal, so an operator-requested recovery
        // must be a real remount. This is intentionally never done as an
        // automatic periodic retry: a bad card can block synchronous SD I/O.
        SD.end();
        if (!SD.begin(PIN_SD_CS, sharedSpi())) return false;
    }
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);

    // Resolve the run once per power-on, not once per mount, so a card
    // pulled and reseated mid-drive rejoins the run it left rather than
    // splitting one drive across two folders — the gap shows up as `sd`
    // going down and back in this run's own health rows.
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
        if (runFilePath(probePath, sizeof(probePath), LOG_DIR, runIndex, PROBE_LEAF) == 0) {
            return false;
        }
        if (runFilePath(energyPath, sizeof(energyPath), LOG_DIR, runIndex, ENERGY_LEAF) == 0) {
            return false;
        }
        if (runFilePath(nodesPath, sizeof(nodesPath), LOG_DIR, runIndex, NODES_LEAF) == 0) {
            return false;
        }
    }

    if (!ensureCsvLocked(detectionsPath, LOG_CSV_HEADER)) return false;
    // A missing health log must not stop detections being logged: the
    // detections are the mission, this is instrumentation.
    ensureCsvLocked(sessionPath, SESSION_CSV_HEADER);
    // Probe observations are durable mission output when a Probe is run.
    // Creating the empty file at boot keeps the run directory schema stable.
    if (!ensureCsvLocked(probePath, SCAN_CSV_HEADER)) return false;
    // Same durability tier as probePath: Sweep is mission output too, and
    // DESIGN.md requires a durable scan to refuse starting if its output
    // file cannot be opened.
    if (!ensureCsvLocked(energyPath, ENERGY_CSV_HEADER)) return false;
    if (!ensureCsvLocked(nodesPath, NODE_CSV_HEADER)) return false;
    return true;
}

// The two failure modes need different handling by the caller, so they are
// distinguished rather than collapsed into a bool: a busy bus means "try
// again shortly", a file error means "the card is gone".
enum class WriteResult { OK, BUS_BUSY, FILE_ERROR };

// Appends `len` bytes to `path` in exactly one open/write/close, acquiring
// the bus itself. The caller must NOT already hold the bus.
//
// `worstMs` is the caller's own high-water mark, tracked PER WRITER: an
// early hardware run charged a boot health-row's 26ms to the detection-
// flush metric (`flushes=0 maxflush=26ms`) when they shared one counter.
// "Is the logger starving the radio?" is the max across both; "is my batch
// sizing wrong?" is only about detection flushes — merging them answered
// the first and broke the second.
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

    const MemorySnapshot memory = memoryStatsSnapshot();
    s.heap_free = memory.heap_free;
    s.heap_min = memory.heap_min;
    s.heap_largest = memory.heap_largest;
    s.heap_free_blocks = memory.heap_free_blocks;
    s.heap_allocated_blocks = memory.heap_allocated_blocks;
    s.batt_mv = batteryMilliVolts();
    s.radio_stack_free = memoryTaskStackFree(memory, MemoryTask::RADIO);
    s.gps_stack_free = memoryTaskStackFree(memory, MemoryTask::GPS);
    s.logger_stack_free = memoryTaskStackFree(memory, MemoryTask::LOGGER);
    s.ui_stack_free = memoryTaskStackFree(memory, MemoryTask::UI);
    s.wifi_stack_free = memoryTaskStackFree(memory, MemoryTask::WIFI);
    s.scan_observations = radioScanObservationCount();
    s.scan_observation_drops = radioScanObservationDropCount() + loggerScanRowsDropped();
    s.probe_runs = radioDiscoverySweepCount();
    s.probe_cancels = radioDiscoveryCancelCount();
    s.probe_timeouts = radioDiscoveryTimeoutCount();
    s.probe_failures = radioDiscoveryFailureCount();
    s.probe_recoveries = radioDiscoveryRecoveryCount();
    s.probe_last_away_ms = radioDiscoveryLastAwayMs();
    s.energy_observations = radioEnergyObservationCount();
    s.energy_observation_drops = radioEnergyObservationDropCount() + loggerEnergyRowsDropped();
    s.identities_decoded = radioIdentityDecodeCount();
    s.identity_drops = radioIdentityDropCount() + loggerIdentityRowsDropped();

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

    char row[DETECTION_CSV_MAX_ROW];
    size_t n = detectionFormatCsv(det, row, sizeof(row), timestamp, fresh, fix.lat, fix.lon,
                                  haveFix ? fix.fix_quality : 0, runIndex);
    if (n == 0) {
        rowsDropped++;
        return;
    }

    // Verbose debug mode (ui_task.cpp menu toggle): reuses the exact CSV
    // row already built above, so it never drifts from what actually lands
    // on SD. Printed regardless of `sdReady` — for bench sessions that
    // want live RX detail without an SD card or the WiFi AP in the loop.
    //
    // Keep the lock across the three writes: main.cpp's [status] line runs
    // on Core 1, and raw-frame rows can no longer fit in one small scratch
    // buffer. The lock, not the number of writes, prevents torn lines.
    if (debugVerbose && !serialControlIsEnabled()) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) {
            serialWriteAll((const uint8_t *)"[debug] ", 8);
            serialWriteAll((const uint8_t *)row, n);
            serialWriteAll((const uint8_t *)"\n", 1);
        }
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

void appendScanObservation(const ScanObservation &observation) {
    if (!sdReady) {
        scanRowsDropped++;
        return;
    }

    GpsFix fix;
    const bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(50));
    const uint32_t now = millis();
    const bool fresh = haveFix && gpsFixIsFresh(fix, now, FIX_MAX_AGE_MS);

    char timestamp[24];
    detectionFormatTimestamp(timestamp, sizeof(timestamp), haveFix && fix.has_time, fix.year,
                             fix.month, fix.day, fix.hour, fix.minute, fix.second);

    char row[256];
    const size_t n = scanObservationFormatCsv(
        observation, row, sizeof(row), timestamp, fresh, fix.lat, fix.lon,
        haveFix ? fix.fix_quality : 0, runIndex);
    if (n == 0) {
        scanRowsDropped++;
        return;
    }
    row[n] = '\n';

    const WriteResult result = appendToFile(probePath, row, n + 1, maxScanMs);
    if (result == WriteResult::OK) {
        scanRowsWritten++;
    } else {
        scanRowsDropped++;
        if (result == WriteResult::FILE_ERROR) sdReady = false;
    }
}

// Sweep peak observations are deliberately a separate queue and file, same
// reasoning as appendScanObservation() above: not a packet, must never
// change RX/log counters.
void appendEnergyObservation(const EnergyObservation &observation) {
    if (!sdReady) {
        energyRowsDropped++;
        return;
    }

    GpsFix fix;
    const bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(50));
    const uint32_t now = millis();
    const bool fresh = haveFix && gpsFixIsFresh(fix, now, FIX_MAX_AGE_MS);

    char timestamp[24];
    detectionFormatTimestamp(timestamp, sizeof(timestamp), haveFix && fix.has_time, fix.year,
                             fix.month, fix.day, fix.hour, fix.minute, fix.second);

    char row[256];
    const size_t n = energyObservationFormatCsv(
        observation, row, sizeof(row), timestamp, fresh, fix.lat, fix.lon,
        haveFix ? fix.fix_quality : 0, runIndex);
    if (n == 0) {
        energyRowsDropped++;
        return;
    }
    row[n] = '\n';

    const WriteResult result = appendToFile(energyPath, row, n + 1, maxEnergyMs);
    if (result == WriteResult::OK) {
        energyRowsWritten++;
    } else {
        energyRowsDropped++;
        if (result == WriteResult::FILE_ERROR) sdReady = false;
    }
}

void appendNodeIdentity(const NodeIdentity &identity) {
    if (!sdReady) {
        identityRowsDropped++;
        return;
    }
    GpsFix fix;
    const bool haveFix = gpsGetFix(fix, pdMS_TO_TICKS(50));
    const uint32_t now = millis();
    const bool fresh = haveFix && gpsFixIsFresh(fix, now, FIX_MAX_AGE_MS);
    char timestamp[24];
    detectionFormatTimestamp(timestamp, sizeof(timestamp), haveFix && fix.has_time, fix.year,
                             fix.month, fix.day, fix.hour, fix.minute, fix.second);
    char row[NODE_IDENTITY_CSV_MAX_ROW];
    const size_t n = nodeIdentityFormatCsv(identity, row, sizeof(row), timestamp, fresh,
                                           fix.lat, fix.lon, haveFix ? fix.fix_quality : 0, runIndex);
    if (n == 0) {
        identityRowsDropped++;
        return;
    }
    row[n] = '\n';
    // Keep this separate from Probe's CAD-write metric: identity writes are
    // normal wardrive logging, not evidence about Probe's bounded mode.
    const WriteResult result = appendToFile(nodesPath, row, n + 1, maxIdentityMs);
    if (result == WriteResult::OK) {
        identityRowsWritten++;
    } else {
        identityRowsDropped++;
        if (result == WriteResult::FILE_ERROR) sdReady = false;
    }
}

void loggerTask(void *) {
    memoryStatsRegisterCurrentTask(MemoryTask::LOGGER);
    // The config reader mounted the card a moment ago during setup(). Keep
    // that mount live on the normal boot path. If it failed, remain offline
    // and drain/drop queued records; an absent or sick card is never allowed
    // to create a boot-time remount loop.
    if (initialSdMounted) {
        SpiBusLock lock(portMAX_DELAY);
        if (lock.held()) sdReady = openLogsLocked(false);
    }

    uint32_t lastFlush = millis();
    uint32_t lastSession = millis();
    bool bootRowWritten = false;

    for (;;) {
        Detection det;
        // Wake either on a detection or on the flush interval, whichever
        // comes first — so a quiet period still commits buffered rows.
        if (xQueueReceive(detectionQueue, &det, pdMS_TO_TICKS(100)) == pdTRUE) {
            appendDetection(det);
        }

        // CAD observations are deliberately a separate queue and file. They
        // are not packet detections and must never change RX/log counters.
        if (scanObservationQueue != nullptr) {
            ScanObservation observation;
            if (xQueueReceive(scanObservationQueue, &observation, 0) == pdTRUE) {
                appendScanObservation(observation);
            }
        }

        // Sweep peak observations, same non-blocking drain shape as Probe's
        // above — a separate queue and file, never packet detections.
        if (energyObservationQueue != nullptr) {
            EnergyObservation observation;
            if (xQueueReceive(energyObservationQueue, &observation, 0) == pdTRUE) {
                appendEnergyObservation(observation);
            }
        }

        if (identityQueue != nullptr) {
            NodeIdentity identity;
            if (xQueueReceive(identityQueue, &identity, 0) == pdTRUE) appendNodeIdentity(identity);
        }

        const uint32_t now = millis();
        if (batchLen >= BATCH_HIGH_WATER || (batchLen > 0 && now - lastFlush >= FLUSH_INTERVAL_MS)) {
            flushBatch();
            lastFlush = now;
        }

        // A recovery attempt is operator-triggered from System > Retry SD.
        // SD/FatFS calls are synchronous, so repeatedly invoking them on a
        // physically failed card can starve this Core and turn an SD fault
        // into a watchdog reboot loop. One deliberate retry after a reseat
        // is recoverable; unattended retries are not.
        if (!sdReady && sdRetryRequested) {
            sdRetryRequested = false;
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) sdReady = openLogsLocked(true);
        }

        // Health row last, so it samples counters that include this pass's
        // work. The boot row fires as soon as the card is available rather
        // than a minute in, so an early-dying run still says when it
        // started; a later remount doesn't repeat it — `sd` flipping down
        // and back in the periodic rows is what a mid-drive reseat looks
        // like.
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

bool loggerTaskStart(QueueHandle_t queue, QueueHandle_t scanQueue, QueueHandle_t energyQueue,
                     QueueHandle_t nodesQueue, bool mountedAtBoot) {
    detectionQueue = queue;
    scanObservationQueue = scanQueue;
    energyObservationQueue = energyQueue;
    identityQueue = nodesQueue;
    initialSdMounted = mountedAtBoot;
    sdReady = false;
    sdRetryRequested = false;
    // Core 0, priority 2: above GPS (1) so rows drain promptly, below the
    // radio (3) which must always win.
    //
    // 5120 rather than 4096: the health-row path (GpsFix + SessionStats +
    // a 320-byte row buffer) is a deeper frame than the detection path and
    // calls into SD/FatFS from the bottom of it. Cheap margin on ~330KB
    // free heap; logger_stack_free in session.csv reports the real number.
    BaseType_t ok = xTaskCreatePinnedToCore(loggerTask, "logger", 5120, nullptr, 2, nullptr, 0);
    loggerStarted = ok == pdPASS;
    return ok == pdPASS;
}

bool loggerSdReady() {
    return sdReady;
}
bool loggerRequestSdRetry() {
    if (!loggerStarted || sdReady) return false;
    sdRetryRequested = true;
    return true;
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
uint32_t loggerScanRowsWritten() {
    return scanRowsWritten;
}
uint32_t loggerScanRowsDropped() {
    return scanRowsDropped;
}
uint32_t loggerEnergyRowsWritten() {
    return energyRowsWritten;
}
uint32_t loggerEnergyRowsDropped() {
    return energyRowsDropped;
}
uint32_t loggerIdentityRowsWritten() {
    return identityRowsWritten;
}
uint32_t loggerIdentityRowsDropped() {
    return identityRowsDropped;
}

void loggerDebugToggle() {
    debugVerbose = !debugVerbose;
    if (debugVerbose && !serialControlIsEnabled()) {
        // Column header once on enable, so the CSV-shaped lines that follow
        // are readable rather than a wall of unlabeled commas. One locked
        // Serial call — an earlier unlocked version of this exact line
        // came out torn on hardware (see serial_lock.h).
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[debug] verbose mode ON\n[debug] %s\n", LOG_CSV_HEADER);
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held() && n > 0) {
            serialWriteAll((const uint8_t *)buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
        }
    } else if (!serialControlIsEnabled()) {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) serialPrintln("[debug] verbose mode OFF");
    }
}
bool loggerDebugIsEnabled() {
    return debugVerbose;
}
