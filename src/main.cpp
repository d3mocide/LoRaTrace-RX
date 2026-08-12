// LoRaTrace RX — Phase 1 bring-up (DESIGN.md §9, step 1)
//
// Scope, deliberately: RadioLib talking to the SX1262 on its own SPI host,
// IO-expander antenna switch confirmed, hardcoded RX on the Meshtastic
// LongFast (US) channel, detections printed to Serial. No task/queue
// architecture, no GPS, no SD, no UI yet — CLAUDE.md is explicit that
// those wait until this bring-up step is proven on real hardware.
//
// UNTESTED ON HARDWARE: written and reasoned through against RadioLib's
// documented API and the M5Stack/PI4IOE5V6408 facts recorded in
// board_pins.h, but this repo has no board attached to flash against. See
// PROGRESS.md "Phase 1" checklist for what still needs bench verification
// before trusting this blind — SPI host availability, IO-expander register
// behavior, and RadioLib's exact call signatures for whatever version
// actually resolves at build time chief among them.

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>

#include "board_pins.h"
#include "channel_plans.h"
#include "version.h"

// Dedicated SPI host for the SX1262, isolated from the display bus
// (DESIGN.md §1: shared-bus display refreshes jitter CAD timing).
SPIClass radioSPI(FSPI);
SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, radioSPI);

volatile bool packetReady = false;

void IRAM_ATTR onPacketReceived() {
    packetReady = true;
}

// Drive P0 on the PI4IOE5V6408 high once at boot. Without this the radio
// is silent regardless of everything else being correct (DESIGN.md §1).
// Every register is written explicitly rather than relying on power-on
// defaults — see board_pins.h for the verify status of this register map.
bool initAntennaSwitch() {
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

    // Drive P0 high.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(IOEXP_REG_OUTPUT_STATE);
    Wire.write(antMask);
    if (Wire.endTransmission() != 0) return false;

    return true;
}

void setup() {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000) {}

    Serial.print(F("LoRaTrace RX v"));
    Serial.print(FIRMWARE_VERSION);
    Serial.println(F(" — phase 1 bring-up"));

    if (!initAntennaSwitch()) {
        Serial.println(F("FATAL: IO expander (antenna switch) init failed — no ACK on I2C. Radio would be silent even if this continued."));
        while (true) delay(1000);
    }
    Serial.println(F("Antenna switch: P0 driven high."));

    radioSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    int state = radio.begin(
        CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz,
        CHANNEL_MESHTASTIC_LONGFAST_US.bw_khz,
        CHANNEL_MESHTASTIC_LONGFAST_US.sf,
        CHANNEL_MESHTASTIC_LONGFAST_US.cr_denom
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("FATAL: SX1262 init failed, code "));
        Serial.println(state);
        while (true) delay(1000);
    }
    Serial.println(F("SX1262 initialized."));
    Serial.print(F("Locked to Meshtastic LongFast (US): "));
    Serial.print(CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz, 3);
    Serial.println(F(" MHz"));

    radio.setDio1Action(onPacketReceived);

    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("FATAL: startReceive failed, code "));
        Serial.println(state);
        while (true) delay(1000);
    }

    Serial.println(F("Listening..."));
}

void loop() {
    if (!packetReady) return;
    packetReady = false;

    uint8_t buf[256];
    size_t len = radio.getPacketLength();
    if (len == 0 || len > sizeof(buf)) {
        radio.startReceive();
        return;
    }

    int state = radio.readData(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
        Serial.print(F("[RX] len="));
        Serial.print(len);
        Serial.print(F(" rssi="));
        Serial.print(radio.getRSSI());
        Serial.print(F("dBm snr="));
        Serial.print(radio.getSNR());
        Serial.println(F("dB"));
    } else {
        Serial.print(F("[RX] read error, code "));
        Serial.println(state);
    }

    // No GPS/SD fusion yet (Phase 2) — this is a serial-only smoke test
    // of the radio path per DESIGN.md §9 step 1.
    radio.startReceive();
}
