#pragma once
// LoRaTrace RX — battery voltage and charge estimate.
//
// Hardware facts SOURCED, not guessed (this project's standing rule):
// m5stack/M5Unified `src/utility/Power_Class.cpp` handles
// `board_M5CardputerADV` explicitly —
//
//     case board_t::board_M5CardputerADV:
//       _batAdcCh   = ADC1_GPIO10_CHANNEL;
//       _batAdcPin  = 10;
//       _pmic       = pmic_t::pmic_adc;
//       _adc_ratio  = 2.0f;
//
// and converts with `mv = raw * _adc_ratio` then
// `level = (mv - 3300) * 100 / (4150 - 3350)`. Both are reproduced below.
// The ADV is named as its own board there, so this is not an assumption
// carried over from the base Cardputer.
//
// Note M5Stack's own docs state Cardputer/Cardputer-Adv **cannot** read
// charge current or charging status — voltage is all the hardware exposes.
// So there is deliberately no isCharging() here: it would have to lie.

#include <stdint.h>

// Battery ADC. Shares nothing with the radio/SD SPI bus or the I2C bus, so
// it needs no arbitration.
constexpr int8_t PIN_BATTERY_ADC = 10;

// On-board divider: the ADC sees half the battery voltage.
constexpr float BATTERY_ADC_RATIO = 2.0f;

// M5Unified's endpoints. The asymmetry (subtract 3300, divide by 800) is
// theirs, reproduced rather than "corrected" so readings agree with what
// every other Cardputer firmware shows for the same battery.
constexpr int32_t BATTERY_MV_EMPTY = 3300;
constexpr int32_t BATTERY_MV_SPAN = 4150 - 3350; // 800

// Converts battery millivolts to 0-100%. Pure, so it's host-testable.
// Clamped at both ends: a LiPo above 4.15V or below 3.3V is off the curve,
// and reporting 112% would be worse than reporting 100%.
inline uint8_t batteryPercentFromMv(uint32_t mv) {
    int32_t level = ((int32_t)mv - BATTERY_MV_EMPTY) * 100 / BATTERY_MV_SPAN;
    if (level < 0) return 0;
    if (level > 100) return 100;
    return (uint8_t)level;
}

// Configures the ADC. Call once from setup().
void batteryInit();

// Battery voltage in millivolts, averaged over several samples. Returns 0
// if the reading is implausible (see battery.cpp) rather than reporting a
// confident wrong number — same principle as refusing to log a stale GPS
// fix or render a no-fix as 0,0.
uint32_t batteryMilliVolts();

// Convenience: batteryPercentFromMv(batteryMilliVolts()). Returns 0 when
// the voltage read failed; pair with batteryMilliVolts() != 0 if you need
// to distinguish "flat" from "unknown".
uint8_t batteryPercent();
