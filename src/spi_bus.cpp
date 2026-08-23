#include "spi_bus.h"

#include "board_pins.h"

namespace {
SemaphoreHandle_t busMutex = nullptr;
volatile uint32_t contention = 0;
SPIClass busSpi(FSPI);
} // namespace

SPIClass &sharedSpi() {
    return busSpi;
}

bool spiBusInit() {
    if (busMutex != nullptr) return true;
    // xSemaphoreCreateMutex (not CreateBinary): priority inheritance is the
    // whole point here — see the header for why that matters on this board.
    busMutex = xSemaphoreCreateMutex();
    if (busMutex == nullptr) return false;
    busSpi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
    return true;
}

bool spiBusTake(TickType_t timeout) {
    if (busMutex == nullptr) return false;
    if (xSemaphoreTake(busMutex, timeout) == pdTRUE) return true;
    contention++;
    return false;
}

void spiBusGive() {
    if (busMutex != nullptr) xSemaphoreGive(busMutex);
}

uint32_t spiBusContentionCount() {
    return contention;
}
