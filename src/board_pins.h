#pragma once
// LoRaTrace RX — pin map and IO-expander register constants.
//
// Pin assignments are transcribed from DESIGN.md §1 (single source of
// truth for hardware decisions). If any of these change, update DESIGN.md
// first, then this file — don't let them drift apart.

#include <cstdint>

// --- SX1262 (Cap LoRa-1262) — dedicated SPI host, isolated from the ---
// --- display bus (shared-bus display refreshes jitter CAD timing).  ---
constexpr int8_t PIN_LORA_NSS  = 5;
constexpr int8_t PIN_LORA_SCK  = 40;
constexpr int8_t PIN_LORA_MOSI = 14;
constexpr int8_t PIN_LORA_MISO = 39;
constexpr int8_t PIN_LORA_IRQ  = 4;  // DIO1
constexpr int8_t PIN_LORA_BUSY = 6;
constexpr int8_t PIN_LORA_RST  = 3;

// --- PI4IOE5V6408 IO expander — antenna-path switch, boot-init only. ---
// The radio hears nothing until P0 is driven high once at boot, regardless
// of radio firmware correctness (DESIGN.md §1).
constexpr int8_t PIN_IOEXP_SDA = 8;
constexpr int8_t PIN_IOEXP_SCL = 9;

// I2C address 0x43 is the PI4IOE5V6408's documented default (ADDR pin tied
// low); confirmed against ESPHome's pi4ioe5v6408 component docs, not yet
// bench-verified against this specific board.
constexpr uint8_t IOEXP_I2C_ADDR = 0x43;

// Register map + bit polarity cross-checked against an independent
// from-datasheet driver (codeberg.org/easytarget/pi4ioe5v6408-micropython),
// not the primary Diodes datasheet directly — TODO(verify) before trusting
// blind on real hardware. Reset/default register values are NOT assumed;
// every register touched below is written explicitly rather than relying
// on power-on defaults.
constexpr uint8_t IOEXP_REG_DEVICE_ID    = 0x01; // ID/reset status, read-only
constexpr uint8_t IOEXP_REG_IO_DIRECTION = 0x03; // bit: 0=input, 1=output
constexpr uint8_t IOEXP_REG_OUTPUT_STATE = 0x05; // bit: 0=low,   1=high
constexpr uint8_t IOEXP_REG_HIGH_Z       = 0x07; // bit: 1=pin high-Z'd (output driver disabled)
constexpr uint8_t IOEXP_REG_PULL_SELECT  = 0x0D; // bit: 0=pull-down, 1=pull-up (only if pull enabled elsewhere)
constexpr uint8_t IOEXP_REG_INPUT_STATUS = 0x0F; // read-only input levels

constexpr uint8_t IOEXP_ANT_SWITCH_BIT = 0; // P0 — antenna path enable

// --- GPS (AT6668) — NMEA over UART. Not wired up until Phase 2. ---
constexpr int8_t PIN_GPS_RX = 13; // ESP32 RX <- GPS TX
constexpr int8_t PIN_GPS_TX = 15; // ESP32 TX -> GPS RX
constexpr uint32_t GPS_BAUD = 115200;

// --- microSD — bus TBD. Pins intentionally omitted. ---
// DESIGN.md §7 flags "SPI or SDMMC, confirm host" as an unresolved item on
// this board revision. Don't guess a pin assignment here; fill in once
// confirmed against real hardware (see PROGRESS.md open questions).
