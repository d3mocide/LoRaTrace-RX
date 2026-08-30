#pragma once
// LoRaTrace RX — pin map and IO-expander register constants.
// Pin assignments are transcribed from docs/DESIGN.md §1 (source of truth). If
// any change, update docs/DESIGN.md first, then this file — don't let them drift.

#include <cstdint>

// SX1262 (Cap LoRa-1262) — dedicated SPI host, isolated from the display
// bus (a shared bus would jitter CAD timing).
constexpr int8_t PIN_LORA_NSS  = 5;
constexpr int8_t PIN_LORA_SCK  = 40;
constexpr int8_t PIN_LORA_MOSI = 14;
constexpr int8_t PIN_LORA_MISO = 39;
constexpr int8_t PIN_LORA_IRQ  = 4;  // DIO1
constexpr int8_t PIN_LORA_BUSY = 6;
constexpr int8_t PIN_LORA_RST  = 3;

// PI4IOE5V6408 IO expander — antenna-path switch, boot-init only. The
// radio hears nothing until P0 is driven high once at boot (docs/DESIGN.md §1).
constexpr int8_t PIN_IOEXP_SDA = 8;
constexpr int8_t PIN_IOEXP_SCL = 9;

// I2C addr 0x43: confirmed via ESPHome's pi4ioe5v6408 docs and the Cap
// LoRa-1262 module's own silkscreen ("PI4IO ADDR:0x43").
constexpr uint8_t IOEXP_I2C_ADDR = 0x43;

// Register map/polarity cross-checked against an independent from-datasheet
// driver (codeberg.org/easytarget/pi4ioe5v6408-micropython), not the Diodes
// datasheet directly — TODO(verify). Every register is written explicitly;
// power-on defaults are never assumed.
constexpr uint8_t IOEXP_REG_DEVICE_ID    = 0x01; // ID/reset status, read-only
constexpr uint8_t IOEXP_REG_IO_DIRECTION = 0x03; // bit: 0=input, 1=output
constexpr uint8_t IOEXP_REG_OUTPUT_STATE = 0x05; // bit: 0=low,   1=high
constexpr uint8_t IOEXP_REG_HIGH_Z       = 0x07; // bit: 1=pin high-Z'd (output driver disabled)
constexpr uint8_t IOEXP_REG_PULL_SELECT  = 0x0D; // bit: 0=pull-down, 1=pull-up (only if pull enabled elsewhere)
constexpr uint8_t IOEXP_REG_INPUT_STATUS = 0x0F; // read-only input levels

constexpr uint8_t IOEXP_ANT_SWITCH_BIT = 0; // P0 — antenna path enable

// GPS — NMEA over UART. docs/DESIGN.md calls the chip "AT6668"; M5Stack's own
// docs say "ATGM336H" — naming discrepancy only, doesn't affect these pins.
//
// M5Stack's docs table and its own tutorial code disagree on RX/TX
// polarity. **Resolved on hardware 2026-08-23: the tutorial code is
// right** (RX=G15) — the probe A/B'd both, RX=G15 streamed clean NMEA
// (80 sentences/5s, 0 checksum errors), RX=G13 produced nothing. Docs
// table was likely labeled from the host's perspective, not the GPS's —
// trust running code over a scraped pin table.
//
// GPS_BAUD 115200 agrees across both sources, not in doubt.
//
// **The GPS is powered by IO-expander P0**, same pin as the antenna
// switch (io_expander.h) — a "dead" GPS on a correct UART is usually
// just unpowered.
constexpr int8_t PIN_GPS_RX = 15; // ESP32 RX <- GPS TX
constexpr int8_t PIN_GPS_TX = 13; // ESP32 TX -> GPS RX
constexpr uint32_t GPS_BAUD = 115200;

// Swapped ordering, kept named so the GPS probe can A/B both automatically
// rather than needing a reflash to test a hunch.
constexpr int8_t PIN_GPS_RX_ALT = 13;
constexpr int8_t PIN_GPS_TX_ALT = 15;

// microSD — SPI, sharing the SAME bus as the SX1262 above. Confirmed via
// two M5Stack docs pages (Cardputer-Adv's microSD table, Cap LoRa-1262's
// SPI pin table): SD is CS=G12/SCK=G40/MOSI=G14/MISO=G39, SX1262 is
// NSS=G5 on the identical SCK/MOSI/MISO — one shared bus via chip-select,
// because the microSD interface shares pins with the EXT/Cap connector the
// LoRa Cap plugs into. Resolves docs/DESIGN.md §7's "SPI or SDMMC?" in favor of
// SPI, shared.
//
// Implication: SD and radio SPI can't truly run concurrently regardless of
// which core issues them — Core-0-only SD keeps the *radio task's own
// code* from blocking, but real bus arbitration (spi_bus.h's mutex) is
// still required once both are active.
constexpr int8_t PIN_SD_CS = 12;
// SCK/MOSI/MISO: reuse PIN_LORA_SCK / PIN_LORA_MOSI / PIN_LORA_MISO.

// ST7789V2 LCD (135x240, IPS) — own SPI host, isolated from the radio/SD
// bus (same jitter-avoidance reasoning as above). Pins + IPS offsets taken
// from bmorcelli/Launcher's confirmed-working Cardputer-ADV config, not
// derived from a datasheet. Bench-verified since Phase 1 and every Phase 6
// UI session — panel renders correctly at these offsets.
constexpr int8_t PIN_TFT_MOSI = 35;
constexpr int8_t PIN_TFT_SCLK = 36;
// Write-only display, so SPI is begun with MISO = -1. ESP32-S3 has no
// default HSPI MISO pin, so this logs a benign ERROR every boot
// (`spiAttachMISO(): HSPI Does not have default pins on ESP32S3!`) —
// confirmed harmless on hardware (2026-08-23): spiAttachMISO() just
// returns without attaching anything, which is the desired outcome.
// Left unsilenced on purpose: the only fix is handing the bus a real GPIO
// as MISO, which would misrepresent the pin map for a line this device
// doesn't use — a worse legacy than one noisy boot line.
constexpr int8_t PIN_TFT_CS   = 37;
constexpr int8_t PIN_TFT_DC   = 34;
constexpr int8_t PIN_TFT_RST  = 33;
constexpr int8_t PIN_TFT_BL   = 38; // backlight — LEDC PWM (backlight.h/.cpp), not a plain digitalWrite any more

constexpr int16_t TFT_PANEL_WIDTH  = 135;
constexpr int16_t TFT_PANEL_HEIGHT = 240;
// Arduino_ST7789's constructor takes two (col,row) offset pairs. **Not**
// "pair 1 portrait, pair 2 landscape" — per Arduino_GFX v1.4.0's actual
// setRotation() switch, rotation 1 (ours) mixes ROW_OFFSET1/COL_OFFSET2,
// not a clean pair. So `main.cpp` must pass all four constants below in
// order even though only rotation 1 is used — omitting the second pair
// silently defaults it to 0 (the cause of a real screen-clear glitch,
// 2026-08-22). Names kept as PORTRAIT/LANDSCAPE because the numeric values
// match this ST7789 135x240 IPS panel's well-known offset table; sourced
// from Launcher's confirmed-working config.
constexpr uint8_t TFT_COL_OFFSET_PORTRAIT  = 52;
constexpr uint8_t TFT_ROW_OFFSET_PORTRAIT  = 40;
constexpr uint8_t TFT_COL_OFFSET_LANDSCAPE = 53;
constexpr uint8_t TFT_ROW_OFFSET_LANDSCAPE = 40;

// TCA8418 keyboard controller (addr 0x34) shares this I2C bus with the
// PI4IOE5V6408 above (same SDA/SCL, different address) — Cardputer ADV
// only, base Cardputer uses a GPIO matrix instead. Sourced from
// bmorcelli/Launcher's Cardputer-ADV config (addr 0x34, SDA 8, SCL 9,
// INT 11, `tca.matrix(7, 8)`). **Boots in SLEEP and reports no keypress
// until explicitly configured**, same trap class as the GPS power rail —
// Adafruit_TCA8418's begin()+matrix() does that wake sequence.
constexpr uint8_t KEYBOARD_I2C_ADDR = 0x34;
constexpr int8_t PIN_KEYBOARD_INT = 11; // active-low; unused (ui_task polls)
constexpr uint8_t KEYBOARD_MATRIX_ROWS = 7;
constexpr uint8_t KEYBOARD_MATRIX_COLS = 8; // 7x8 = the ADV's 56 keys

// Not a conflict — normal shared-I2C-bus operation, confirmed by this
// repo's first hardware boot. bmorcelli/Launcher's own Cardputer-ADV notes
// say GPIO5 (= PIN_LORA_NSS) needs to be driven HIGH early to avoid SD-mount
// interference from this I2C cluster; not needed here since our boot log
// already shows both the antenna-switch writes and SD mount succeeding
// without it. Recorded in case SD/radio flakiness shows up later.
