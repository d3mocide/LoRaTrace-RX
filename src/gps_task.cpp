#include "gps_task.h"

#include <Arduino.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/time.h>

#include "board_pins.h"
#include "memory_stats.h"
#include "nmea.h"
#include "serial_lock.h"

namespace {

HardwareSerial gpsSerial(1); // UART1; UART0 is the USB-CDC console

SemaphoreHandle_t fixMutex = nullptr;
GpsFix sharedFix; // guarded by fixMutex

volatile uint32_t sentenceCount = 0;
volatile uint32_t checksumErrors = 0;
// Longest gap between two passes of gpsTask()'s drain loop. A direct,
// mechanism-level measurement for the nmea_bad_crc investigation
// (PROGRESS.md): this task is lowest priority on Core 0 (deliberately —
// see gpsTaskStart()), so a busy logger holding the CPU while it writes to
// SD is the leading theory for where bytes get lost. If this stays small
// even while flushes are happening, that theory is wrong and the noise is
// coming from somewhere else (wiring, RF coupling, the module itself).
volatile uint32_t maxLoopGapMs = 0;
// Lines discarded for overrunning lineBuf before a terminator arrived — see
// gps_task.h for why this is worth tracking separately from checksumErrors.
volatile uint32_t oversizeDrops = 0;
// millis() at the first position fix of this power-on; 0 = none yet.
// Captured here rather than derived downstream because time-to-first-fix is
// only meaningful relative to boot, and by the time the logger sees a fix
// it has no way to know whether that fix is one second or one hour old as
// a *session* event.
volatile uint32_t firstFixMs = 0;
// The ESP32's system clock starts at the epoch and nothing else ever sets
// it, so until GPS supplies a date every file written to the card is stamped
// 1980 (FAT's own epoch, which is what a 1970 system time clamps to). That
// makes a card full of runs impossible to order by anything but its
// contents. GPS time arrives well before a position fix does — it needs one
// satellite, not four — so this lands early enough to be useful.
bool systemTimeSet = false;

// Line assembly buffer. Task-local (not static inside the loop) so its
// lifetime is obvious; sized by NMEA's 82-char spec limit.
char lineBuf[NMEA_MAX_SENTENCE];
size_t lineLen = 0;

void handleSentence(const char *s) {
    if (!nmeaChecksumValid(s)) {
        checksumErrors++;
        return;
    }
    sentenceCount++;

    // Parse into a local first, then publish under the mutex. This keeps
    // the critical section to a single struct copy instead of holding the
    // lock across the whole parse — the logger only ever waits microseconds.
    GpsFix updated;
    if (xSemaphoreTake(fixMutex, portMAX_DELAY) == pdTRUE) {
        updated = sharedFix;
        xSemaphoreGive(fixMutex);
    }

    const uint32_t now = millis();
    if (!gpsApplySentence(updated, s, now)) return;

    if (updated.has_position && firstFixMs == 0) firstFixMs = now;

    // Once per power-on: adopt GPS UTC as the system clock so SD file
    // timestamps stop reading 1980. Deliberately NOT gated on a position
    // fix — time is valid without one, and waiting for position would leave
    // the files wrong for the whole acquisition window.
    if (!systemTimeSet) {
        const int64_t epoch = gpsFixToEpoch(updated);
        if (epoch > 0) {
            struct timeval tv = {};
            tv.tv_sec = (time_t)epoch;
            if (settimeofday(&tv, nullptr) == 0) {
                systemTimeSet = true;
                // One buffer, one locked Serial call — same pattern as
                // main.cpp's [status] line, for the same reason
                // (serial_lock.h): this races against every other task's
                // own prints just like any other cross-core Serial write.
                char line[80];
                snprintf(line, sizeof(line),
                         "[gps] system clock set from GPS: %u-%u-%u %u:%u UTC — SD file "
                         "timestamps are real from here.",
                         (unsigned)updated.year, (unsigned)updated.month, (unsigned)updated.day,
                         (unsigned)updated.hour, (unsigned)updated.minute);
                SerialLock lock(pdMS_TO_TICKS(200));
                if (lock.held()) Serial.println(line);
            }
        }
    }

    if (xSemaphoreTake(fixMutex, portMAX_DELAY) == pdTRUE) {
        sharedFix = updated;
        xSemaphoreGive(fixMutex);
    }
}

void gpsTask(void *) {
    memoryStatsRegisterCurrentTask(MemoryTask::GPS);
    // Default ring buffer is 256 bytes. At ~17 sentences/sec x ~75 bytes
    // (5-constellation output, measured 2026-08-23) that's under 200ms of
    // slack before an unread buffer starts dropping bytes. Bumped to 1024
    // as cheap insurance against exactly the CPU-starvation scenario
    // maxLoopGapMs (below) exists to detect — must be called before begin().
    gpsSerial.setRxBufferSize(1024);
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    uint32_t lastLoopMs = millis();

    for (;;) {
        const uint32_t loopStart = millis();
        const uint32_t gap = loopStart - lastLoopMs;
        if (gap > maxLoopGapMs) maxLoopGapMs = gap;
        lastLoopMs = loopStart;

        bool didWork = false;
        while (gpsSerial.available()) {
            didWork = true;
            char c = (char)gpsSerial.read();
            if (c == '\n' || c == '\r') {
                if (lineLen > 0) {
                    lineBuf[lineLen] = '\0';
                    handleSentence(lineBuf);
                    lineLen = 0;
                }
                continue;
            }
            if (lineLen + 1 < sizeof(lineBuf)) {
                lineBuf[lineLen++] = c;
            } else {
                lineLen = 0; // oversize/garbled — resync at the next newline
                oversizeDrops++;
            }
        }

        // NMEA arrives at 1Hz in bursts, so there is nothing to do most of
        // the time. Sleeping rather than spinning keeps this task off the
        // CPU that the logger shares. 20ms is well inside the gap between
        // bursts even at 10Hz update rates, and the UART FIFO covers the
        // interval regardless.
        vTaskDelay(pdMS_TO_TICKS(didWork ? 5 : 20));
    }
}

} // namespace

bool gpsTaskStart() {
    if (fixMutex == nullptr) {
        fixMutex = xSemaphoreCreateMutex();
        if (fixMutex == nullptr) return false;
    }

    // Core 0 per DESIGN.md §2; Core 1 is reserved for the radio task alone.
    // Priority 1: this is the least latency-sensitive of the three tasks —
    // a fix is only sampled once per detection, and GPS updates at 1Hz.
    BaseType_t ok = xTaskCreatePinnedToCore(gpsTask, "gps", 3072, nullptr, 1, nullptr, 0);
    return ok == pdPASS;
}

bool gpsGetFix(GpsFix &out, TickType_t timeout) {
    if (fixMutex == nullptr) return false;
    if (xSemaphoreTake(fixMutex, timeout) != pdTRUE) return false;
    out = sharedFix;
    xSemaphoreGive(fixMutex);
    return true;
}

uint32_t gpsSentenceCount() {
    return sentenceCount;
}

uint32_t gpsChecksumErrorCount() {
    return checksumErrors;
}

uint32_t gpsMaxLoopGapMs() {
    return maxLoopGapMs;
}

uint32_t gpsOversizeDropCount() {
    return oversizeDrops;
}

uint32_t gpsFirstFixMillis() {
    return firstFixMs;
}

bool gpsSystemTimeSet() {
    return systemTimeSet;
}
