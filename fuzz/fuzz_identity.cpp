// Audit A27: the two identity decoders, fed arbitrary bytes. These parse
// payloads that arrived over the air and produce the names and public keys
// that end up in nodes.csv, so they are the most directly attacker-reachable
// parsers in the firmware. The varint reader is included deliberately: it used
// to accept an overflowing tenth byte.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/meshcore_identity.h"
#include "../src/meshtastic_identity.h"

// Coverage note: meshtasticDecodeDefaultNodeIdentity() is NOT exercised here.
// It lives in meshtastic_identity.cpp and needs mbedtls AES, which is an
// ESP-IDF component with no host headers on a stock Linux box. Its varint
// reader and the shared CSV formatter are header-only and are covered; the
// AES-CTR decode path is not, and no claim is made that it is.

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;

    // Raw varint reader over the whole input, at every start offset.
    for (size_t start = 0; start < size && start < 16; start++) {
        size_t pos = start;
        uint64_t value = 0;
        meshtasticReadVarint(data, size, pos, value);
        if (pos > size) __builtin_trap(); // must never read past the buffer
    }

    Detection det = {};
    const size_t rawLen = (size - 1) > DETECTION_RAW_MAX_LEN ? DETECTION_RAW_MAX_LEN : (size - 1);
    if (!detectionSetRawPacket(det, data + 1, rawLen)) return 0;

    for (uint8_t profile = 0; profile < 4; profile++) {
        det.profile = profile;
        NodeIdentity identity;
        if (!meshcoreDecodeAdvertIdentity(det, identity)) continue;
        // Anything the decoders claim must be NUL-terminated inside its own
        // buffer: these strings go straight into a CSV row.
        if (memchr(identity.long_name, '\0', sizeof(identity.long_name)) == nullptr) {
            __builtin_trap();
        }
        if (memchr(identity.short_name, '\0', sizeof(identity.short_name)) == nullptr) {
            __builtin_trap();
        }
        static char row[NODE_IDENTITY_CSV_MAX_ROW + 32];
        memset(row, 0xAA, sizeof(row));
        const size_t n = nodeIdentityFormatCsv(identity, row, NODE_IDENTITY_CSV_MAX_ROW,
                                               "2026-09-07T00:00:00Z", true, 45.5, -122.6, 1, 7);
        if (n > 0) {
            if (n >= NODE_IDENTITY_CSV_MAX_ROW) __builtin_trap();
            if (row[n] != '\0') __builtin_trap();
            for (size_t i = NODE_IDENTITY_CSV_MAX_ROW; i < sizeof(row); i++) {
                if ((uint8_t)row[i] != 0xAA) __builtin_trap();
            }
        }
    }
    return 0;
}
