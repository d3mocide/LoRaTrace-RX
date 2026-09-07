#pragma once
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include "channel_plans.h"
#include "config_line.h"

inline bool channelBandwidthValid(float value) {
    const float allowed[] = {7.8f,10.4f,15.6f,20.8f,31.25f,41.7f,62.5f,125.0f,250.0f,500.0f};
    for (float bw : allowed) if (fabsf(value - bw) < 0.01f) return true;
    return false;
}
inline bool channelConfigValid(const ChannelParams &p) {
    return isfinite(p.freq_mhz) && p.freq_mhz >= 868 && p.freq_mhz <= 928 &&
           p.sf >= 5 && p.sf <= 12 && p.cr_denom >= 5 && p.cr_denom <= 8 &&
           isfinite(p.bw_khz) && channelBandwidthValid(p.bw_khz);
}
inline bool channelApplyValue(ChannelParams &p, const char *field, const char *value) {
    if (!field || !value || !*value) return false;
    ChannelParams next = p;
    if (!strcmp(field, "freq_mhz") || !strcmp(field, "bw_khz")) {
        char *end = nullptr; errno = 0;
        float v = strtof(value, &end);
        if (end == value || *end || errno || !isfinite(v)) return false;
        if (!strcmp(field, "freq_mhz")) next.freq_mhz = v; else next.bw_khz = v;
    } else {
        char *end = nullptr; errno = 0;
        const bool sync = !strcmp(field, "sync_word");
        long v = strtol(value, &end, sync ? 0 : 10);
        if (end == value || *end || errno || v < 0 || v > 255) return false;
        if (sync) next.sync_word = (uint8_t)v;
        else if (!strcmp(field, "sf")) next.sf = (uint8_t)v;
        else if (!strcmp(field, "cr_denom")) next.cr_denom = (uint8_t)v;
        else return false;
    }
    if (!channelConfigValid(next)) return false;
    p = next;
    return true;
}
inline bool channelApplyConfigLine(const char *line, ProfileOverrides &overrides) {
    char key[48], value[48];
    if (!configLineSplit(line, key, sizeof(key), value, sizeof(value))) return false;
    const bool mt = strncmp(key, "meshtastic_", 11) == 0;
    const bool mc = strncmp(key, "meshcore_", 9) == 0;
    if (!mt && !mc) return false;
    const MissionProfile profile = mt ? MissionProfile::MESHTASTIC : MissionProfile::MESHCORE;
    ChannelParams p = resolvedChannelForProfile(overrides, profile);
    if (!channelApplyValue(p, key + (mt ? 11 : 9), value)) return false;
    if (mt) { overrides.meshtastic = p; overrides.meshtastic_set = true; }
    else { overrides.meshcore = p; overrides.meshcore_set = true; }
    return true;
}
