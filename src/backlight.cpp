#include "backlight.h"

#include <Arduino.h>
#include <stdio.h>

#include "board_pins.h"
#include "serial_lock.h"

namespace {

// Nothing else in this codebase uses LEDC — channel 0 is free with no
// collision risk.
constexpr uint8_t BACKLIGHT_LEDC_CHANNEL = 0;

// 2026-08-25 bench pass: hand-rolled linear PWM (first at 20kHz, then
// 1kHz) blacked the display out at various brightness levels instead of
// dimming it — not a simple "wrong frequency" bug, since 25% failed at
// 1kHz right after 50%/75% had just started working there. Root cause
// found by checking how M5Stack's own board-support code drives this
// exact pin: M5GFX.cpp's autodetect branch for board_M5CardputerADV calls
// `_set_pwm_backlight(GPIO_NUM_38, ch, /*freq=*/256, /*invert=*/false,
// /*offset=*/16)` — GPIO 38 is PIN_TFT_BL, so this is *this* hardware, not
// a generic reference. Two things were wrong in the original version here:
//   1. Wrong frequency (20kHz/1kHz vs. the validated 256Hz).
//   2. A naive linear 0-100% -> 0-max duty mapping, when LovyanGFX's
//      Light_PWM::setBrightness() (github.com/m5stack/M5GFX,
//      src/lgfx/v1/platforms/esp32/Light_PWM.cpp) uses a non-linear curve
//      built around that `offset` specifically to keep low/mid brightness
//      values above whatever duty this backlight driver needs to stay in
//      regulation — a linear map has no such floor, so most non-boundary
//      values could fall below it depending on the driver's real
//      threshold, which is exactly the "some values collapse" symptom
//      hit twice tonight.
// What follows below is that formula and those exact constants, not a
// new guess — replicated rather than reinvented, since this project has
// no way to independently characterize the driver IC and M5Stack already
// has for this precise board.
constexpr uint32_t BACKLIGHT_PWM_FREQ_HZ = 256;
constexpr uint8_t BACKLIGHT_PWM_RESOLUTION_BITS = 9; // Light_PWM's PWM_BITS
constexpr uint32_t BACKLIGHT_PWM_MAX_DUTY = (1u << BACKLIGHT_PWM_RESOLUTION_BITS) - 1; // 511
constexpr uint8_t BACKLIGHT_OFFSET = 16; // Light_PWM's config_t::offset for this board
constexpr bool BACKLIGHT_INVERT = false; // this board's config_t::invert

// Direct port of Light_PWM::setBrightness()'s duty curve (brightness is
// 0-255, matching that function's own input range) — deliberately not
// simplified or re-derived, see the block comment above for why.
uint32_t brightnessToDuty(uint8_t brightness) {
    uint32_t duty = 0;
    if (brightness != 0) {
        uint32_t ofs = BACKLIGHT_OFFSET;
        if (ofs) ofs = ofs * 259 >> 8;
        duty = (uint32_t)brightness * (257 - ofs);
        duty += ofs * 255;
        duty += 1u << (15 - BACKLIGHT_PWM_RESOLUTION_BITS);
        duty >>= 16 - BACKLIGHT_PWM_RESOLUTION_BITS;
    }
    if (BACKLIGHT_INVERT) duty = BACKLIGHT_PWM_MAX_DUTY + 1 - duty;
    if (duty > BACKLIGHT_PWM_MAX_DUTY) duty = BACKLIGHT_PWM_MAX_DUTY; // ledcWrite() should already clamp; belt and suspenders
    return duty;
}

} // namespace

void backlightInit() {
    const uint32_t actualFreq = ledcSetup(BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_PWM_FREQ_HZ, BACKLIGHT_PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_TFT_BL, BACKLIGHT_LEDC_CHANNEL);
    char line[64];
    snprintf(line, sizeof(line), "[backlight] init: requested %luHz, got %luHz",
             (unsigned long)BACKLIGHT_PWM_FREQ_HZ, (unsigned long)actualFreq);
    {
        SerialLock lock(pdMS_TO_TICKS(200));
        if (lock.held()) serialPrintln(line);
    }
    backlightSetPercent(100);
}

void backlightSetPercent(uint8_t pct) {
    if (pct > 100) pct = 100;
    const uint8_t brightness = (uint8_t)((uint32_t)pct * 255 / 100);
    const uint32_t duty = brightnessToDuty(brightness);
    ledcWrite(BACKLIGHT_LEDC_CHANNEL, duty);

    char line[64];
    snprintf(line, sizeof(line), "[backlight] set: pct=%u brightness=%u duty=%lu/%lu",
             (unsigned)pct, (unsigned)brightness, (unsigned long)duty, (unsigned long)BACKLIGHT_PWM_MAX_DUTY);
    SerialLock lock(pdMS_TO_TICKS(200));
    if (lock.held()) serialPrintln(line);
}
