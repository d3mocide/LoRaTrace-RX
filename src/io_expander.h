#pragma once
// LoRaTrace RX — PI4IOE5V6408 IO-expander init.
//
// P0 does TWO things on the Cap LoRa-1262, and missing either one produces
// a silent, confusing failure:
//
//   1. Enables the RF antenna switch. Without it the radio hears nothing
//      regardless of firmware correctness (DESIGN.md §1).
//   2. **Powers the GPS module.** M5Stack's own Arduino example for this
//      Cap drives expander pin 0 high with the comment that it enables GPS
//      power, and the LoRa868 Cap (no GPS) omits the call entirely.
//
// Point 2 was learned the expensive way on 2026-08-23: the standalone GPS
// probe deliberately isolated itself from the rest of the boot sequence to
// make its failure mode unambiguous — and in doing so skipped this init,
// leaving the GPS unpowered and reporting zero UART bytes. Isolation is
// still the right instinct for a bring-up tool, but the power rail is not
// one of the variables worth isolating.
//
// Extracted into its own translation unit precisely so that every binary in
// this repo (firmware, GPS probe, and the Phase 2 tasks) shares one
// implementation instead of each growing its own copy to forget.

#include <stdint.h>

// Brings up I2C on the expander's bus and drives P0 high. Idempotent enough
// to call once per boot from setup(). Returns false if any I2C write fails
// to ACK — the caller decides whether that's fatal (it is for the radio).
bool ioExpanderInit();
