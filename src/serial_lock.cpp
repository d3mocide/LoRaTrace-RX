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

    // USB Serial/JTAG has a 64-byte endpoint/FIFO boundary. Feeding a long
    // frame in one write can lose the first packet on ESP32-S3 while the
    // driver still reports the full count (observed on 70-byte STATUS
    // frames). Keep the mutex across these smaller writes so chunks from
    // another task cannot interleave.
    constexpr size_t USB_WRITE_CHUNK = 32;
    size_t written = 0;
    uint32_t lastProgress = millis();
    while (written < length) {
        const size_t remaining = length - written;
        const size_t request = remaining < USB_WRITE_CHUNK ? remaining : USB_WRITE_CHUNK;
        const size_t n = Serial.write(data + written, request);
        if (n > 0) {
            written += n;
            lastProgress = millis();
            // Give the USB Serial/JTAG ISR a scheduling opportunity between
            // endpoint-sized chunks. Without this pause, back-to-back frame
            // chunks can be acknowledged by the driver yet one whole chunk
            // is occasionally absent at the host under boot/log load.
            // Tried Serial.flush() here instead of delay(1) on 2026-08-28
            // (docs/history/CHANGELOG.md): no measured improvement against the same
            // repro, and it risks blocking the caller — and this whole
            // critical section — for longer if the host isn't draining.
            // Reverted; the driver drops chunks (sometimes the first,
            // sometimes mid/last) regardless of write pacing, so this stays
            // a real driver-level limitation, not a timing bug software can
            // close from this side. See serialPrintln()'s always-terminate
            // fix for the one piece that is fixable here.
            if (written < length) delay(1);
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
    // Always attempt the terminator, even when the body came up short: a
    // truncated-but-terminated line lets the host's line reader (and this
    // protocol's CRC check) reject one bad line and resync on the next.
    // Skipping the terminator here left a dangling open line on the wire
    // that the following writer's bytes ran directly into, producing a
    // single unparseable hybrid line instead of two separable ones
    // (docs/history/CHANGELOG.md, 2026-08-28 STATUS-response repro).
    const bool bodyOk = serialWriteAll((const uint8_t *)line, length) == length;
    static const uint8_t newline[] = {'\r', '\n'};
    const bool newlineOk = serialWriteAll(newline, sizeof(newline)) == sizeof(newline);
    return bodyOk && newlineOk;
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
