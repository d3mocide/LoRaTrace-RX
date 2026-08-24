#include "serial_lock.h"

namespace {
SemaphoreHandle_t serialMutex = nullptr;
} // namespace

bool serialLockInit() {
    if (serialMutex != nullptr) return true;
    // xSemaphoreCreateMutex (not CreateBinary): priority inheritance, same
    // reasoning as spi_bus.h — a low-priority task holding this briefly
    // shouldn't let a mid-priority task starve a high-priority one out of
    // the console entirely.
    serialMutex = xSemaphoreCreateMutex();
    return serialMutex != nullptr;
}

bool serialLockTake(TickType_t timeout) {
    if (serialMutex == nullptr) return false;
    return xSemaphoreTake(serialMutex, timeout) == pdTRUE;
}

void serialLockGive() {
    if (serialMutex != nullptr) xSemaphoreGive(serialMutex);
}
