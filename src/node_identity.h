#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "channel_plans.h"

constexpr size_t NODE_IDENTITY_LONG_NAME_MAX = 40;
constexpr size_t NODE_IDENTITY_SHORT_NAME_MAX = 8;
constexpr size_t NODE_IDENTITY_PUBLIC_KEY_LEN = 32;
constexpr size_t NODE_IDENTITY_CSV_MAX_ROW = 384;

struct NodeIdentity {
    uint32_t rx_millis = 0;
    uint32_t node_id = 0;
    uint16_t raw_len = 0;
    uint8_t profile = (uint8_t)MissionProfile::MESHTASTIC;
    uint8_t node_type = 0;  // MeshCore advert type; 0 = not supplied
    char long_name[NODE_IDENTITY_LONG_NAME_MAX] = {};
    char short_name[NODE_IDENTITY_SHORT_NAME_MAX] = {};
    uint8_t public_key[NODE_IDENTITY_PUBLIC_KEY_LEN] = {};
    bool has_public_key = false;
};

static_assert(sizeof(NodeIdentity) <= 96, "Node identity queue budget exceeded");

constexpr const char *NODE_CSV_HEADER =
    "timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,node_id,"
    "node_type,long_name,short_name,public_key_hex,raw_len";

inline const char *nodeIdentityTypeName(uint8_t type) {
    switch (type) {
        case 1: return "chat";
        case 2: return "repeater";
        case 3: return "room_server";
        case 4: return "sensor";
        default: return "";
    }
}

inline void nodeIdentityCopyString(char *out, size_t outSize, const uint8_t *value, size_t valueLen) {
    if (outSize == 0) return;
    const size_t copyLen = valueLen < outSize - 1 ? valueLen : outSize - 1;
    for (size_t i = 0; i < copyLen; i++) {
        // Identity text is untrusted radio data. Keep it to one CSV line;
        // quotes are escaped by nodeIdentityAppendCsvField().
        const uint8_t byte = value[i];
        out[i] = (byte < 0x20 || byte == 0x7F) ? ' ' : (char)byte;
    }
    out[copyLen] = '\0';
}

inline bool nodeIdentityAppendCsvField(char *out, size_t outSize, size_t &used, const char *value) {
    if (value == nullptr) value = "";
    if (used + 2 >= outSize) return false;
    out[used++] = '"';
    for (const char *p = value; *p; p++) {
        if (*p == '"') {
            if (used + 2 >= outSize) return false;
            out[used++] = '"';
        }
        if (used + 1 >= outSize) return false;
        out[used++] = *p;
    }
    if (used + 2 >= outSize) return false;
    out[used++] = '"';
    out[used++] = ',';
    return true;
}

inline size_t nodeIdentityFormatCsv(const NodeIdentity &identity, char *out, size_t outSize,
                                    const char *timestamp_utc, bool has_fix, double lat, double lon,
                                    uint8_t fix_quality, uint16_t run) {
    if (out == nullptr || outSize == 0 ||
        (identity.node_id == 0 && !identity.has_public_key)) return 0;
    char latbuf[16], lonbuf[16];
    if (has_fix) {
        snprintf(latbuf, sizeof(latbuf), "%.6f", lat);
        snprintf(lonbuf, sizeof(lonbuf), "%.6f", lon);
    } else {
        latbuf[0] = '\0';
        lonbuf[0] = '\0';
    }
    const bool isMeshCore = identity.profile == (uint8_t)MissionProfile::MESHCORE;
    int n = isMeshCore
        ? snprintf(out, outSize, "%s,%s,%s,%u,%u,%lu,%s,#%02lx,",
                   timestamp_utc ? timestamp_utc : "", latbuf, lonbuf,
                   (unsigned)fix_quality, (unsigned)run,
                   (unsigned long)identity.rx_millis,
                   missionProfileName(identity.profile),
                   (unsigned long)(identity.node_id & 0xFF))
        : snprintf(out, outSize, "%s,%s,%s,%u,%u,%lu,%s,!%08lx,",
                   timestamp_utc ? timestamp_utc : "", latbuf, lonbuf,
                   (unsigned)fix_quality, (unsigned)run,
                   (unsigned long)identity.rx_millis,
                   missionProfileName(identity.profile),
                   (unsigned long)identity.node_id);
    if (n < 0 || (size_t)n >= outSize) return 0;
    size_t used = (size_t)n;
    if (!nodeIdentityAppendCsvField(out, outSize, used, nodeIdentityTypeName(identity.node_type)) ||
        !nodeIdentityAppendCsvField(out, outSize, used, identity.long_name) ||
        !nodeIdentityAppendCsvField(out, outSize, used, identity.short_name)) return 0;
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    if (identity.has_public_key) {
        for (uint8_t byte : identity.public_key) {
            if (used + 3 >= outSize) return 0;
            out[used++] = HEX_DIGITS[byte >> 4];
            out[used++] = HEX_DIGITS[byte & 0x0F];
        }
    }
    int tail = snprintf(out + used, outSize - used, ",%u", (unsigned)identity.raw_len);
    if (tail < 0 || (size_t)tail >= outSize - used) return 0;
    return used + (size_t)tail;
}
