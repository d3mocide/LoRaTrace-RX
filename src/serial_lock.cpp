#include "serial_lock.h"

#include <Arduino.h>
#include <string.h>

namespace {
SemaphoreHandle_t serialMutex = nullptr;
} // namespace

bool serialLockInit() {
    if (serialMutex != nullptr) return true;
    // xSemaphoreCreateMutex (not CreateBinary): priority inheritance, same
    // reasoning as spi_bus.h — a low-priority task holding this briefly
    // shouldn't let a mid-priority task starve a high-priority one out of
    // the console entirely.
    serialMutex = xSemaphoreCreateMutex();
    return serialMutex != nullptr;
}

bool serialLockTake(TickType_t timeout) {
    if (serialMutex == nullptr) return false;
    return xSemaphoreTake(serialMutex, timeout) == pdTRUE;
}

void serialLockGive() {
    if (serialMutex != nullptr) xSemaphoreGive(serialMutex);
}

size_t serialWriteAll(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) return 0;

    size_t written = 0;
    uint32_t lastProgress = millis();
    while (written < length) {
        const size_t n = Serial.write(data + written, length - written);
        if (n > 0) {
            written += n;
            lastProgress = millis();
            continue;
        }
        // A disconnected or wedged USB host must never stall a task forever.
        // The normal HWCDC write timeout is 100ms; allow a few retries for a
        // transient full ring, then report the short count to the caller.
        if (millis() - lastProgress >= 500) break;
        delay(1);
    }
    return written;
}

bool serialPrintln(const char *line) {
    const size_t length = line != nullptr ? strlen(line) : 0;
    if (serialWriteAll((const uint8_t *)line, length) != length) return false;
    static const uint8_t newline[] = {'\r', '\n'};
    return serialWriteAll(newline, sizeof(newline)) == sizeof(newline);
}

void serialLockDrain() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    // HWCDC::flush() intentionally discards its queued bytes when the USB
    // host is not connected.  Do not turn a transient enumeration gap into
    // guaranteed log loss; let the ring drain once the host is ready.
    if (!Serial.isConnected()) return;
#endif
    Serial.flush();
}
