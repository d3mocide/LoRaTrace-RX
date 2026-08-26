#pragma once

#include <stddef.h>
#include <stdint.h>

enum class MemoryTask : uint8_t {
    RADIO = 0,
    GPS,
    LOGGER,
    UI,
    WIFI,
    COUNT,
};

struct MemorySnapshot {
    uint32_t heap_free = 0;
    uint32_t heap_min = 0;
    uint32_t heap_largest = 0;
    uint32_t heap_free_blocks = 0;
    uint32_t heap_allocated_blocks = 0;
    uint32_t stack_free[(size_t)MemoryTask::COUNT] = {};
};

// Registers the calling FreeRTOS task for later stack high-water sampling.
void memoryStatsRegisterCurrentTask(MemoryTask task);

// Captures internal 8-bit heap health and every registered task's minimum
// remaining stack. A zero stack value means that task has not started yet.
MemorySnapshot memoryStatsSnapshot();

uint32_t memoryTaskStackFree(const MemorySnapshot &snapshot, MemoryTask task);

// One bounded Serial line for lifecycle checkpoints; never used per packet.
void memoryStatsLog(const char *event);
