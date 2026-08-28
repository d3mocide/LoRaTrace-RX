#include "memory_stats.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "logger_task.h"
#include "low_profile.h"
#include "serial_lock.h"

namespace {

TaskHandle_t taskHandles[(size_t)MemoryTask::COUNT] = {};

} // namespace

void memoryStatsRegisterCurrentTask(MemoryTask task) {
    const size_t index = (size_t)task;
    if (index >= (size_t)MemoryTask::COUNT) return;
    taskHandles[index] = xTaskGetCurrentTaskHandle();
}

MemorySnapshot memoryStatsSnapshot() {
    MemorySnapshot snapshot;
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Preserve session.csv's existing ESP.get*Heap() semantics so Phase 7
    // rows remain comparable with every pre-Phase-7 hardware run.
    snapshot.heap_free = ESP.getFreeHeap();
    snapshot.heap_min = ESP.getMinFreeHeap();
    snapshot.heap_largest = (uint32_t)info.largest_free_block;
    snapshot.heap_free_blocks = (uint32_t)info.free_blocks;
    snapshot.heap_allocated_blocks = (uint32_t)info.allocated_blocks;

    for (size_t i = 0; i < (size_t)MemoryTask::COUNT; ++i) {
        TaskHandle_t handle = taskHandles[i];
        if (handle != nullptr) {
            snapshot.stack_free[i] =
                (uint32_t)uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t);
        }
    }
    return snapshot;
}

uint32_t memoryTaskStackFree(const MemorySnapshot &snapshot, MemoryTask task) {
    const size_t index = (size_t)task;
    return index < (size_t)MemoryTask::COUNT ? snapshot.stack_free[index] : 0;
}

void memoryStatsLog(const char *event) {
    if (!loggerDebugIsEnabled() || lowProfileIsEnabled()) return;
    const MemorySnapshot s = memoryStatsSnapshot();
    char line[256];
    snprintf(line, sizeof(line),
             "[mem] %s free=%lu min=%lu largest=%lu blocks=%lu/%lu "
             "stack=radio:%lu,gps:%lu,logger:%lu,ui:%lu,wifi:%lu",
             event ? event : "snapshot", (unsigned long)s.heap_free,
             (unsigned long)s.heap_min, (unsigned long)s.heap_largest,
             (unsigned long)s.heap_free_blocks, (unsigned long)s.heap_allocated_blocks,
             (unsigned long)memoryTaskStackFree(s, MemoryTask::RADIO),
             (unsigned long)memoryTaskStackFree(s, MemoryTask::GPS),
             (unsigned long)memoryTaskStackFree(s, MemoryTask::LOGGER),
             (unsigned long)memoryTaskStackFree(s, MemoryTask::UI),
             (unsigned long)memoryTaskStackFree(s, MemoryTask::WIFI));

    SerialLock lock(pdMS_TO_TICKS(200));
    if (lock.held()) serialPrintln(line);
}
