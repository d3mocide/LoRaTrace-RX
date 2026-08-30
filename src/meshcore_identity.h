#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "detection.h"
#include "node_identity.h"

// MeshCore v1 packet + advert layout, source: meshcore-dev/MeshCore
// docs/packet_format.md and docs/payloads.md. Adverts are signed but this
// receiver records the signed observation; it does not claim signature
// verification on this constrained target.
constexpr uint8_t MESHCORE_PAYLOAD_ADVERT = 0x04;
constexpr size_t MESHCORE_ADVERT_FIXED_BYTES = 32 + 4 + 64;

inline bool meshcoreDecodeAdvertIdentity(const Detection &det, NodeIdentity &out) {
    if (det.profile != (uint8_t)MissionProfile::MESHCORE || det.raw_len < 2) return false;

    const uint8_t header = det.raw_packet[0];
    if ((header >> 6) != 0 || ((header >> 2) & 0x0F) != MESHCORE_PAYLOAD_ADVERT) return false;

    size_t pos = 1;
    const uint8_t routeType = header & 0x03;
    if (routeType == 0 || routeType == 3) pos += 4;  // two uint16 transport codes
    if (pos >= det.raw_len) return false;
    const uint8_t pathInfo = det.raw_packet[pos++];
    const uint8_t hashSizeCode = pathInfo >> 6;
    if (hashSizeCode == 3) return false;
    const size_t pathLen = (size_t)(pathInfo & 0x3F) * (hashSizeCode + 1);
    if (pathLen > det.raw_len - pos || MESHCORE_ADVERT_FIXED_BYTES > det.raw_len - pos - pathLen) {
        return false;
    }
    pos += pathLen;

    out = {};
    out.rx_millis = det.rx_millis;
    out.node_id = det.raw_packet[pos];  // MeshCore node hash = first public-key byte
    out.raw_len = det.raw_len;
    out.profile = (uint8_t)MissionProfile::MESHCORE;
    memcpy(out.public_key, det.raw_packet + pos, NODE_IDENTITY_PUBLIC_KEY_LEN);
    out.has_public_key = true;
    pos += MESHCORE_ADVERT_FIXED_BYTES;  // public key, timestamp, signature

    if (pos == det.raw_len) return true;  // a valid advert with no appdata
    const uint8_t flags = det.raw_packet[pos++];
    out.node_type = flags & 0x0F;
    if (flags & 0x10) {
        if (det.raw_len - pos < 8) return false;
        pos += 8;  // lat/lon belong to the node's advert, not this receiver's GPS fix
    }
    if (flags & 0x20) {
        if (det.raw_len - pos < 2) return false;
        pos += 2;
    }
    if (flags & 0x40) {
        if (det.raw_len - pos < 2) return false;
        pos += 2;
    }
    if ((flags & 0x80) && pos < det.raw_len) {
        nodeIdentityCopyString(out.long_name, sizeof(out.long_name), det.raw_packet + pos,
                               det.raw_len - pos);
    }
    return true;
}
