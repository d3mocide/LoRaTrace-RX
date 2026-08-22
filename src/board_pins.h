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

// I2C address 0x43, confirmed two ways: ESPHome's pi4ioe5v6408 component
// docs (generic default), and directly silkscreened on the Cap LoRa-1262
// module itself ("PI4IO ADDR:0x43") per its official pin-diagram image —
// about as confirmed as a value can be without a multimeter on the board.
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

// --- GPS — NMEA over UART. Not wired up until Phase 2. ---
// DESIGN.md names the chip "AT6668"; M5Stack's own Cap LoRa-1262 docs and
// pin diagram say "ATGM336H" — a naming discrepancy worth fixing in
// DESIGN.md, doesn't affect these pins (plain NMEA over UART either way).
// G13/G15 confirmed directly against the Cap LoRa-1262 docs pin table and
// its official pin-diagram image, not just DESIGN.md's own table.
constexpr int8_t PIN_GPS_RX = 13; // ESP32 RX <- GPS TX
constexpr int8_t PIN_GPS_TX = 15; // ESP32 TX -> GPS RX
constexpr uint32_t GPS_BAUD = 115200;

// --- microSD — SPI, sharing the SAME bus as the SX1262 above. ---
// Confirmed by cross-checking two of M5Stack's own official docs pages
// directly (Cardputer-Adv base unit's microSD table, and the Cap
// LoRa-1262's own SPI pin table + its printed pin-diagram image): SD is
// CS=G12/SCK=G40/MOSI=G14/MISO=G39, the SX1262 is NSS=G5 on the identical
// SCK/MOSI/MISO — one shared physical bus via chip-select, not two
// independent hosts. The Cardputer-Adv docs state outright that the
// microSD interface shares pins with the EXT/Cap expansion connector,
// which is the connector the LoRa Cap plugs into — that's the actual
// mechanism, not a numeric coincidence. Resolves DESIGN.md §7's "SPI or
// SDMMC?" question in favor of SPI, shared. Still not confirmed with an
// actual continuity/multimeter check on this exact board, but this is
// well-sourced now, not a guess.
//
// Implication for Phase 2: since these are the same physical wires, SD
// and radio SPI transactions can't truly happen concurrently no matter
// which core issues them — moving SD to a Core 0 task avoids the *radio
// task's own code* blocking on SD, but doesn't remove the need for actual
// bus arbitration (e.g. a mutex around the shared SPIClass) once both are
// active at once. Flagged in PROGRESS.md; not solved here.
constexpr int8_t PIN_SD_CS = 12;
// SCK/MOSI/MISO: reuse PIN_LORA_SCK / PIN_LORA_MOSI / PIN_LORA_MISO.

// --- ST7789V2 LCD (135x240, IPS) — own SPI host, isolated from the ---
// --- radio/SD bus above (DESIGN.md §1: shared-bus display refreshes ---
// --- jitter CAD timing; this is the isolation that rule is about).  ---
// DESIGN.md §1's hardware table listed this as "internal" (unspecified)
// pending Phase 6. Pulled forward here for the Phase 1 boot-status splash
// (PROGRESS.md decisions log) — pins and IPS column/row offsets are NOT
// independently bench-verified against this board; they're taken directly
// from a project confirmed working on real Cardputer/Cardputer-ADV
// hardware (bmorcelli/Launcher, boards/m5stack-cardputer/platformio.ini,
// which explicitly targets "M5Stack Cardputer & ADV" as one env), not
// guessed or derived. TODO(verify) once this splash is bench-tested here.
constexpr int8_t PIN_TFT_MOSI = 35;
constexpr int8_t PIN_TFT_SCLK = 36;
constexpr int8_t PIN_TFT_CS   = 37;
constexpr int8_t PIN_TFT_DC   = 34;
constexpr int8_t PIN_TFT_RST  = 33;
constexpr int8_t PIN_TFT_BL   = 38; // backlight, driven digital HIGH (no dimming yet)

constexpr int16_t TFT_PANEL_WIDTH  = 135;
constexpr int16_t TFT_PANEL_HEIGHT = 240;
// Arduino_GFX's Arduino_ST7789 constructor takes two (col,row) offset
// pairs for this panel's IPS glass. **Not** "pair 1 for portrait, pair 2
// for landscape" as originally assumed here — checked against the actual
// GFX Library for Arduino v1.4.0 source (Arduino_TFT.cpp setRotation())
// 2026-08-22 after a real screen-clear glitch, and its switch statement
// mixes one value from each pair for the landscape rotations:
//   rotation 0: xStart=COL_OFFSET1,   yStart=ROW_OFFSET1
//   rotation 1: xStart=ROW_OFFSET1,   yStart=COL_OFFSET2   <- ours
//   rotation 2: xStart=COL_OFFSET2,   yStart=ROW_OFFSET2
//   rotation 3: xStart=ROW_OFFSET2,   yStart=COL_OFFSET1
// So `main.cpp` must pass all four constants below (col_offset1,
// row_offset1, col_offset2, row_offset2, in that order) even though only
// rotation 1 is ever used — omitting the second pair silently defaults it
// to 0, which is exactly what caused the glitch. The two PORTRAIT/
// LANDSCAPE-named pairs below are really "rotation-0 set" and
// "rotation-2 set" in the library's terms; the names are kept because the
// numeric values match the well-known offset table for this common
// ST7789 135x240 IPS panel (also used in e.g. TTGO T-Display) once fed
// into the constructor correctly. Values still sourced from Launcher's
// confirmed-working config, not independently derived from a datasheet.
constexpr uint8_t TFT_COL_OFFSET_PORTRAIT  = 52;
constexpr uint8_t TFT_ROW_OFFSET_PORTRAIT  = 40;
constexpr uint8_t TFT_COL_OFFSET_LANDSCAPE = 53;
constexpr uint8_t TFT_ROW_OFFSET_LANDSCAPE = 40;

// --- PI4IOE5V6408 also shares this device's I2C bus with a TCA8418 ---
// --- keyboard controller (addr 0x34) on Cardputer ADV specifically  ---
// --- (base Cardputer uses a plain GPIO matrix instead) — same SDA/SCL ---
// --- pins (G8/G9) as IOEXP_I2C_ADDR above, different device address. ---
// Not a conflict (that's normal shared-I2C-bus operation, confirmed by
// this repo's own first hardware boot: antenna-switch init succeeded, see
// PROGRESS.md). Noted here because bmorcelli/Launcher's own Cardputer-ADV
// notes (boards/m5stack-cardputer/CardputerADV.md) flag that, on this
// board revision, GPIO5 needs to be driven HIGH during early GPIO init to
// avoid SD-mount interference from this same I2C device cluster — GPIO5
// is PIN_LORA_NSS here. No action taken: our own boot log already shows
// both the antenna-switch I2C writes and the SD mount succeeding without
// that workaround, so it isn't blocking us in practice. Recorded in case
// SD/radio flakiness ever shows up later and this is worth revisiting.
