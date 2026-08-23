#include "io_expander.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

// Every register is written explicitly rather than relying on power-on
// defaults — see board_pins.h for the verify status of this register map.
// This is a verbatim move of main.cpp's former initAntennaSwitch(), which
// was confirmed working on real hardware (all three writes ACK at 0x43,
// antenna path live, packets received) before being lifted here; the only
// change is the name, which now reflects that P0 also powers the GPS.
bool ioExpanderInit() {
    Wire.begin(PIN_IOEXP_SDA, PIN_IOEXP_SCL);

    const uint8_t antMask = 1 << IOEXP_ANT_SWITCH_BIT;

    // P0 = output, leave other pins as inputs.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_IO_DIRECTION);
    Wire.write(antMask);
    if (Wire.endTransmission() != 0) return false;

    // Disable high-Z on P0 so the output actually drives the pin.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_HIGH_Z);
    Wire.write(static_cast<uint8_t>(~antMask)); // 0 = not high-Z for P0
    if (Wire.endTransmission() != 0) return false;

    // Drive P0 high: antenna switch enabled, GPS powered.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_OUTPUT_STATE);
    Wire.write(antMask);
    if (Wire.endTransmission() != 0) return false;

    return true;
}
