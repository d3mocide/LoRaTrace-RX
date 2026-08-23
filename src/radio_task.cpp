#include "radio_task.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/task.h>

#include "board_pins.h"
#include "spi_bus.h"

namespace {

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, sharedSpi());

TaskHandle_t radioTaskHandle = nullptr;
QueueHandle_t detectionQueue = nullptr;
ChannelParams activeChannel;

int lastError = RADIOLIB_ERR_NONE;

volatile uint32_t packetCount = 0;
volatile uint32_t crcErrorCount = 0;
volatile uint32_t queueDropCount = 0;
volatile uint32_t busMissCount = 0;

// How long the radio task will wait for the shared SPI bus. Generous enough
// to ride out a normal SD flush, short enough that a wedged logger can't
// take the receiver down with it. On timeout we drop this packet and keep
// listening — see the header for why that ordering is deliberate.
constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(250);

// DIO1 fires on RX-done. Keep this to a notification and nothing else: no
// SPI, no Serial, no allocation. RadioLib requires the ISR be IRAM-safe.
void IRAM_ATTR onDio1Action() {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (radioTaskHandle != nullptr) {
        vTaskNotifyGiveFromISR(radioTaskHandle, &higherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void radioTask(void *) {
    uint8_t buf[256];

    for (;;) {
        // Block until DIO1 says a packet landed. The timeout is a liveness
        // safety net, not an expected path: if an interrupt is ever missed,
        // this re-checks and re-arms rather than deafening the receiver
        // permanently.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) radio.startReceive();
            continue;
        }

        Detection det = {};
        bool haveDetection = false;

        {
            // Everything touching the SX1262 happens inside this one short
            // critical section. Order matters and mirrors the Phase 1 fix:
            // read the stats, then re-arm RX *before* doing anything slow,
            // because the chip is deaf between DIO1 firing and startReceive().
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                continue;
            }

            size_t len = radio.getPacketLength();
            int state = RADIOLIB_ERR_NONE;
            if (len > 0 && len <= sizeof(buf)) {
                state = radio.readData(buf, len);
            } else {
                state = RADIOLIB_ERR_UNKNOWN;
            }

            float rssi = 0.0f, snr = 0.0f;
            if (state == RADIOLIB_ERR_NONE) {
                // Must be read before re-arming: GetPacketStatus reports the
                // *last* packet, so a new arrival would overwrite these.
                rssi = radio.getRSSI();
                snr = radio.getSNR();
            }

            radio.startReceive();

            if (state == RADIOLIB_ERR_NONE) {
                det.rx_millis = millis();
                det.freq_mhz = activeChannel.freq_mhz;
                det.rssi_dbm = rssi;
                det.snr_db = snr;
                det.raw_len = (uint16_t)len;
                det.bw_khz_x10 = (uint16_t)(activeChannel.bw_khz * 10.0f + 0.5f);
                det.sf = activeChannel.sf;
                det.cr_denom = activeChannel.cr_denom;
                det.sync_word = activeChannel.sync_word;
                det.profile = (uint8_t)MissionProfile::MESHTASTIC;

                // Header fields are protocol-specific. HOME_LISTEN knows
                // which protocol it's locked to, so parse accordingly rather
                // than guessing from the bytes (that's §6 fingerprinting,
                // phase 4). A runt frame leaves these zeroed instead of
                // publishing garbage node ids.
                detectionApplyMeshtasticHeader(det, buf, len);

                packetCount++;
                haveDetection = true;
            } else {
                crcErrorCount++;
            }
        } // bus released here, before any queue work

        if (haveDetection && detectionQueue != nullptr) {
            // Non-blocking by design: never stall the receiver for the log.
            if (xQueueSend(detectionQueue, &det, 0) != pdTRUE) {
                queueDropCount++;
            }
        }
    }
}

} // namespace

bool radioTaskStart(const ChannelParams &channel, QueueHandle_t queue) {
    activeChannel = channel;
    detectionQueue = queue;

    {
        SpiBusLock lock(portMAX_DELAY);
        if (!lock.held()) return false;

        lastError = radio.begin(channel.freq_mhz, channel.bw_khz, channel.sf, channel.cr_denom,
                                channel.sync_word);
        if (lastError != RADIOLIB_ERR_NONE) return false;
    }

    // Create the task before wiring the ISR: onDio1Action dereferences
    // radioTaskHandle, and an interrupt arriving between the two would
    // otherwise find it null (harmless here, but the ordering makes the
    // dependency explicit rather than accidental).
    BaseType_t ok =
        xTaskCreatePinnedToCore(radioTask, "radio", 4096, nullptr, 3, &radioTaskHandle, 1);
    if (ok != pdPASS) return false;

    SpiBusLock lock(portMAX_DELAY);
    if (!lock.held()) return false;
    radio.setDio1Action(onDio1Action);
    lastError = radio.startReceive();
    return lastError == RADIOLIB_ERR_NONE;
}

int radioLastError() {
    return lastError;
}

uint32_t radioPacketCount() {
    return packetCount;
}
uint32_t radioCrcErrorCount() {
    return crcErrorCount;
}
uint32_t radioQueueDropCount() {
    return queueDropCount;
}
uint32_t radioBusMissCount() {
    return busMissCount;
}

ChannelParams radioActiveChannel() {
    return activeChannel; // small POD struct, cheap to return by value
}
