#pragma once
// LoRaTrace RX — arbitration for the SPI bus shared by the SX1262 and the
// microSD card.
//
// Why this exists at all: board_pins.h / docs/DESIGN.md §7 established that the
// radio and the SD card are on the *same physical wires* (SCK G40, MOSI
// G14, MISO G39), distinguished only by chip select. docs/DESIGN.md §2 puts the
// radio on Core 1 and the logger on Core 0, which stops the radio task's
// *code* from blocking on SD — but two devices on one bus still cannot
// transact simultaneously no matter which core issues them. That electrical
// fact is what this mutex covers, and it was flagged as an open question in
// docs/history/PROGRESS.md before Phase 2 started.
//
// A FreeRTOS *mutex* specifically, not a binary semaphore: mutexes carry
// priority inheritance, so when the low-priority logger holds the bus and
// the high-priority radio task wants it, the logger is temporarily boosted
// to release it sooner. With a plain semaphore the radio task could sit
// behind a mid-priority task that isn't even using SPI (classic priority
// inversion), and on this device that translates directly into lost packets.
//
// Contract for callers:
//   * Hold it for a single device transaction, never across a delay, a
//     queue wait, or display I/O.
//   * The radio task must always pass a finite timeout and cope with
//     failure — a missed packet is far better than a wedged receiver.

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The one SPIClass for the radio+SD bus. Owned here rather than in main.cpp
// so that the bus object and the mutex guarding it are impossible to use
// separately — every consumer (radio task, logger task, boot-time config
// read) reaches for the same pair.
//
// The display is deliberately NOT on this bus: it has its own host (HSPI)
// with disjoint pins, which is what docs/DESIGN.md §1's isolation rule is about.
SPIClass &sharedSpi();

// Creates the mutex and begins the SPI peripheral on the shared pins. Call
// once from setup() before starting any task. Returns false if the mutex
// could not be allocated.
bool spiBusInit();

// Blocks up to `timeout` ticks for exclusive use of the shared bus.
bool spiBusTake(TickType_t timeout);
void spiBusGive();

// Count of failed takes, i.e. moments where a task wanted the bus and gave
// up. Non-zero is a real signal that flush sizing needs revisiting, so it's
// surfaced rather than swallowed.
uint32_t spiBusContentionCount();

// Scoped lock. Always check held() — the constructor can legitimately fail
// on timeout, and acting on the bus anyway would corrupt a transfer that
// another task is mid-way through.
class SpiBusLock {
  public:
    explicit SpiBusLock(TickType_t timeout) : held_(spiBusTake(timeout)) {}
    ~SpiBusLock() {
        if (held_) spiBusGive();
    }
    bool held() const { return held_; }

    SpiBusLock(const SpiBusLock &) = delete;
    SpiBusLock &operator=(const SpiBusLock &) = delete;

  private:
    bool held_;
};
