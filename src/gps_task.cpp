#include "gps_task.h"

#include <Arduino.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board_pins.h"
#include "nmea.h"

namespace {

HardwareSerial gpsSerial(1); // UART1; UART0 is the USB-CDC console

SemaphoreHandle_t fixMutex = nullptr;
GpsFix sharedFix; // guarded by fixMutex

volatile uint32_t sentenceCount = 0;
volatile uint32_t checksumErrors = 0;

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

    if (!gpsApplySentence(updated, s, millis())) return;

    if (xSemaphoreTake(fixMutex, portMAX_DELAY) == pdTRUE) {
        sharedFix = updated;
        xSemaphoreGive(fixMutex);
    }
}

void gpsTask(void *) {
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    for (;;) {
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
