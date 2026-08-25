#pragma once
// LoRaTrace RX — ST7789V2 backlight PWM (LEDC).
//
// Before this, PIN_TFT_BL (board_pins.h) was a plain digitalWrite(HIGH) at
// boot and never touched again — on/off only. This switches it to the
// ESP32's LEDC peripheral so ui_task.cpp's Brightness menu and auto-dim-
// on-idle can actually vary the level. Nothing else in this codebase uses
// LEDC, so channel/frequency choices here have no collision risk.
//
// Frequency, resolution, and duty curve (backlight.cpp) aren't guesses —
// they're a direct port of M5Stack's own board-support code for this exact
// pin (M5GFX.cpp's board_M5CardputerADV autodetect branch drives GPIO 38,
// PIN_TFT_BL, at 256Hz with a non-linear offset-compensated curve, not a
// naive linear 0-100% map). Two earlier guesses here — 20kHz, then 1kHz,
// both with a plain linear map — each blacked the display out at some
// brightness levels on real hardware before this was found; see
// backlight.cpp's own comment for the full story.
//
// Deliberately a standalone module rather than living in main.cpp or
// ui_task.cpp: main.cpp needs full brightness for the boot splash before
// ui_task exists, so this can't be private to either file — same reasoning
// as battery.h's own scope.

#include <stdint.h>

// LEDC setup + attach, starts at 100%. Call once from main.cpp's setup(),
// before the boot splash needs to be visible.
void backlightInit();

// Sets brightness as a percentage of full (0-100), clamped. Pure hardware
// primitive — no memory of "the operator's chosen level" vs "currently
// dimmed for idle"; that distinction is ui_task.cpp's concern (it owns all
// the other keyboard/timing state already).
void backlightSetPercent(uint8_t pct);
