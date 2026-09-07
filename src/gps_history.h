#pragma once
#include "gps_parse.h"

// Core-0-only history: a queued observation must never borrow a future fix.
constexpr uint8_t GPS_HISTORY_CAPACITY = 16;
struct GpsHistory {
    GpsFix fixes[GPS_HISTORY_CAPACITY];
    uint8_t next = 0;
    uint8_t count = 0;
};
inline void gpsHistoryPush(GpsHistory &history, const GpsFix &fix) {
    history.fixes[history.next] = fix;
    history.next = (history.next + 1) % GPS_HISTORY_CAPACITY;
    if (history.count < GPS_HISTORY_CAPACITY) ++history.count;
}
inline bool gpsHistoryAt(const GpsHistory &history, uint32_t event_ms, GpsFix &out) {
    for (uint8_t i = 0; i < history.count; ++i) {
        const GpsFix &fix = history.fixes[(history.next + GPS_HISTORY_CAPACITY - 1 - i) % GPS_HISTORY_CAPACITY];
        if ((int32_t)(event_ms - fix.state_updated_ms) >= 0) { out = fix; return true; }
    }
    out = GpsFix{};
    return false;
}
static_assert(sizeof(GpsHistory) <= 2048, "GPS history budget exceeded");
