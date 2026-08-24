#pragma once
// LoRaTrace RX — mutex arbitration for the Serial console.
//
// Why this exists: main.cpp's loop() runs on Core 1 (the same core as
// radio_task), while wifi_task/logger_task/gps_task run on Core 0, and all
// of them print to the same Serial object. ESP32 Arduino's Serial (native
// USB-CDC on this board) has no internal lock protecting concurrent writes
// from different cores.
//
// The 2026-08-23 fix for garbled output (PROGRESS.md) collapsed each
// message to one buffer + one Serial call, on the theory that a single
// call is "far more likely to be atomic" than several. A 2026-08-24
// hardware session proved that theory insufficient under real contention:
// a single Serial.println() from one core was still torn mid-write by
// another core's Serial call landing inside it (the WiFi AP-started line
// and a config-save confirmation both came out with pieces missing, one
// session apart). Fewer calls narrows the window; it doesn't close it.
// This mutex is what actually closes it — every Serial write site takes it
// for the duration of that write.
//
// Mirrors spi_bus.h's SpiBusLock pattern deliberately: same reasoning
// (a real mutex, not a binary semaphore, for priority inheritance; a
// scoped RAII lock that always gets checked with held() rather than
// assumed).
//
// NEVER used by radio_task.cpp: CLAUDE.md's house rule is the radio task
// must never block on non-radio I/O, and it doesn't print at all — keep it
// that way. Adding a print there would need this lock and would violate
// that rule.

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Creates the mutex. Call once from setup(), right after Serial.begin(),
// before any task that might print is started. Returns false if the mutex
// could not be allocated.
bool serialLockInit();

// Blocks up to `timeout` ticks for exclusive use of Serial.
bool serialLockTake(TickType_t timeout);
void serialLockGive();

// Scoped lock. Always check held() before printing — losing one diagnostic
// line to a timeout is far better than a torn one, and far better than
// blocking indefinitely on a console nobody's reading.
class SerialLock {
  public:
    explicit SerialLock(TickType_t timeout) : held_(serialLockTake(timeout)) {}
    ~SerialLock() {
        if (held_) serialLockGive();
    }
    bool held() const { return held_; }

    SerialLock(const SerialLock &) = delete;
    SerialLock &operator=(const SerialLock &) = delete;

  private:
    bool held_;
};
