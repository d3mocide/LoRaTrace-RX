#pragma once

#include <stddef.h>
#include <string.h>

#include "detection.h"
#include "node_identity.h"

constexpr uint32_t MESHTASTIC_PORTNUM_NODEINFO = 4;

inline bool meshtasticReadVarint(const uint8_t *data, size_t len, size_t &pos, uint64_t &value) {
    value = 0;
    for (uint8_t shift = 0; shift < 64 && pos < len; shift += 7) {
        const uint8_t byte = data[pos++];
        value |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

inline bool meshtasticSkipWire(const uint8_t *data, size_t len, size_t &pos, uint8_t wire) {
    uint64_t n = 0;
    switch (wire) {
        case 0: return meshtasticReadVarint(data, len, pos, n);
        case 1: if (pos + 8 > len) return false; pos += 8; return true;
        case 2:
            if (!meshtasticReadVarint(data, len, pos, n) || n > len - pos) return false;
            pos += (size_t)n;
            return true;
        case 5: if (pos + 4 > len) return false; pos += 4; return true;
        default: return false;
    }
}

inline bool meshtasticReadBytesField(const uint8_t *data, size_t len, size_t &pos,
                                     const uint8_t *&value, size_t &valueLen) {
    uint64_t n = 0;
    if (!meshtasticReadVarint(data, len, pos, n) || n > len - pos) return false;
    value = data + pos;
    valueLen = (size_t)n;
    pos += valueLen;
    return true;
}

inline bool meshtasticParseNodeIdentityPayload(const uint8_t *data, size_t len,
                                               uint32_t nodeId, uint32_t rxMillis,
                                               uint16_t rawLen, NodeIdentity &out) {
    const uint8_t *user = nullptr;
    size_t userLen = 0, pos = 0;
    uint64_t portnum = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!meshtasticReadVarint(data, len, pos, tag)) return false;
        const uint32_t field = (uint32_t)(tag >> 3);
        const uint8_t wire = (uint8_t)(tag & 0x07);
        if (field == 1 && wire == 0) {
            if (!meshtasticReadVarint(data, len, pos, portnum)) return false;
        } else if (field == 2 && wire == 2) {
            if (!meshtasticReadBytesField(data, len, pos, user, userLen)) return false;
        } else if (!meshtasticSkipWire(data, len, pos, wire)) {
            return false;
        }
    }
    if (portnum != MESHTASTIC_PORTNUM_NODEINFO || user == nullptr || nodeId == 0) return false;

    out = {};
    out.rx_millis = rxMillis;
    out.node_id = nodeId;
    out.raw_len = rawLen;
    out.profile = (uint8_t)MissionProfile::MESHTASTIC;
    bool any = false;
    pos = 0;
    while (pos < userLen) {
        uint64_t tag = 0;
        if (!meshtasticReadVarint(user, userLen, pos, tag)) return false;
        const uint32_t field = (uint32_t)(tag >> 3);
        const uint8_t wire = (uint8_t)(tag & 0x07);
        if ((field == 2 || field == 3 || field == 8) && wire == 2) {
            const uint8_t *value = nullptr;
            size_t valueLen = 0;
            if (!meshtasticReadBytesField(user, userLen, pos, value, valueLen)) return false;
            if (field == 2) {
                nodeIdentityCopyString(out.long_name, sizeof(out.long_name), value, valueLen);
                any = any || valueLen != 0;
            } else if (field == 3) {
                nodeIdentityCopyString(out.short_name, sizeof(out.short_name), value, valueLen);
                any = any || valueLen != 0;
            } else if (valueLen == NODE_IDENTITY_PUBLIC_KEY_LEN) {
                memcpy(out.public_key, value, NODE_IDENTITY_PUBLIC_KEY_LEN);
                out.has_public_key = true;
                any = true;
            }
        } else if (!meshtasticSkipWire(user, userLen, pos, wire)) {
            return false;
        }
    }
    return any;
}

bool meshtasticDecodeDefaultNodeIdentity(const Detection &det, NodeIdentity &out);
