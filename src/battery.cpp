#include "battery.h"

#include <Arduino.h>

namespace {

// Averaging: the ADC on this part is noisy enough that a single sample
// makes a UI readout jitter by several percent, which reads as a fault to
// anyone watching. Cheap to fix at the source.
constexpr uint8_t SAMPLE_COUNT = 8;

// Sanity window at the *pin* (before the divider is undone). A battery-less
// USB-powered board, a misconfigured ADC, or a floating pin all land well
// outside a real LiPo's range, and returning 0 ("unknown") beats drawing a
// confident 0% or 100%.
constexpr uint32_t PLAUSIBLE_MIN_MV = 2500; // ~2.5V battery — below any usable LiPo
constexpr uint32_t PLAUSIBLE_MAX_MV = 4500; // ~4.5V — above a fully charged LiPo

bool initialised = false;

} // namespace

void batteryInit() {
    // 12dB (formerly 11dB) attenuation gives the widest input range, which
    // is what a ~2.1V half-of-battery reading needs; the default 0dB would
    // saturate around 0.95V and read full-scale forever.
    analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
    initialised = true;
}

uint32_t batteryMilliVolts() {
    if (!initialised) batteryInit();

    // analogReadMilliVolts() applies the chip's factory eFuse ADC
    // calibration, which is what M5Unified does internally too. Using raw
    // analogRead() with a hand-rolled reference constant would be
    // measurably worse and vary part to part.
    uint32_t sum = 0;
    for (uint8_t i = 0; i < SAMPLE_COUNT; i++) {
        sum += analogReadMilliVolts(PIN_BATTERY_ADC);
    }
    const uint32_t pin_mv = sum / SAMPLE_COUNT;

    const uint32_t battery_mv = (uint32_t)(pin_mv * BATTERY_ADC_RATIO);
    if (battery_mv < PLAUSIBLE_MIN_MV || battery_mv > PLAUSIBLE_MAX_MV) return 0;
    return battery_mv;
}

uint8_t batteryPercent() {
    const uint32_t mv = batteryMilliVolts();
    if (mv == 0) return 0;
    return batteryPercentFromMv(mv);
}
