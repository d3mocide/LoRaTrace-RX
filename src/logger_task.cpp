#include "logger_task.h"

#include <Arduino.h>
#include <SD.h>
#include <freertos/task.h>

#include "board_pins.h"
#include "detection.h"
#include "gps_task.h"
#include "spi_bus.h"

namespace {

constexpr const char *LOG_DIR = "/loratrace";
constexpr const char *LOG_PATH = "/loratrace/detections.csv";

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

constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(2000);

QueueHandle_t detectionQueue = nullptr;
bool sdReady = false;

char batchBuf[BATCH_BUF_SIZE];
size_t batchLen = 0;
uint32_t batchRows = 0; // rows currently buffered but not yet on the card

volatile uint32_t rowsWritten = 0;
volatile uint32_t rowsDropped = 0;
volatile uint32_t flushCount = 0;
volatile uint32_t maxFlushMs = 0;

// Mounts SD and ensures the CSV exists with its header row. Assumes the
// caller holds the bus.
bool openLogLocked() {
    if (!SD.begin(PIN_SD_CS, sharedSpi())) return false;
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);

    if (!SD.exists(LOG_PATH)) {
        File f = SD.open(LOG_PATH, FILE_WRITE);
        if (!f) return false;
        f.println(LOG_CSV_HEADER);
        f.close();
    }
    return true;
}

// Appends the batch buffer to SD, acquiring the bus itself. Holds it for
// exactly one open/write/close and nothing more.
void flushBatch() {
    if (batchLen == 0) return;

    uint32_t started = millis();
    {
        SpiBusLock lock(BUS_WAIT);
        if (!lock.held()) {
            // Couldn't get the bus. Keep the data buffered and try again on
            // the next pass rather than discarding it — unlike the radio
            // task, the logger is allowed to be late, just not lossy.
            return;
        }
        File f = SD.open(LOG_PATH, FILE_APPEND);
        if (!f) {
            // The card went away mid-session. Drop this batch and fall back
            // to the retry path; buffering indefinitely would just consume
            // RAM on a device whose datastore is gone.
            sdReady = false;
            rowsDropped += batchRows;
            batchLen = 0;
            batchRows = 0;
            return;
        }
        f.write((const uint8_t *)batchBuf, batchLen);
        f.close();
    }
    uint32_t elapsed = millis() - started;
    if (elapsed > maxFlushMs) maxFlushMs = elapsed;

    flushCount++;
    // Counted here, not at append time: "written" should mean it reached the
    // card, otherwise the diagnostic overstates what survived a power cut.
    rowsWritten += batchRows;
    batchLen = 0;
    batchRows = 0;
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
                                  haveFix ? fix.fix_quality : 0);
    if (n == 0) {
        rowsDropped++;
        return;
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
        if (lock.held()) sdReady = openLogLocked();
    }

    uint32_t lastFlush = millis();

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
            if (lock.held()) sdReady = openLogLocked();
            lastFlush = now;
        }
    }
}

} // namespace

bool loggerTaskStart(QueueHandle_t queue) {
    detectionQueue = queue;
    // Core 0, priority 2: above GPS (1) so log rows drain promptly, below
    // the radio (3) which must always win.
    BaseType_t ok = xTaskCreatePinnedToCore(loggerTask, "logger", 4096, nullptr, 2, nullptr, 0);
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
