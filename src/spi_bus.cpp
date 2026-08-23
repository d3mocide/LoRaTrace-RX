#include "spi_bus.h"

#include "board_pins.h"

namespace {
SemaphoreHandle_t busMutex = nullptr;
volatile uint32_t contention = 0;
} // namespace

// Function-local static, NOT a namespace-scope global. radio_task.cpp
// constructs its SX1262 at namespace scope with `new Module(..., sharedSpi())`,
// which runs during static initialisation — and C++ gives no guarantee about
// the relative order of static initialisation across translation units. A
// plain global here could therefore be handed out before it was constructed.
// A function-local static is guaranteed to be constructed on first call, so
// the reference is always valid no matter who initialises first.
SPIClass &sharedSpi() {
    static SPIClass busSpi(FSPI);
    return busSpi;
}

bool spiBusInit() {
    if (busMutex != nullptr) return true;
    // xSemaphoreCreateMutex (not CreateBinary): priority inheritance is the
    // whole point here — see the header for why that matters on this board.
    busMutex = xSemaphoreCreateMutex();
    if (busMutex == nullptr) return false;
    sharedSpi().begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
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
