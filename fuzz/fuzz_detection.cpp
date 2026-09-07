// Audit A27: the Meshtastic header parser and the detections.csv formatter,
// fed arbitrary bytes. These two see attacker-controlled input on every
// received packet, and the formatter has already had one real off-by-one
// (A05), so the interesting property is not "does it decode" but "does it
// stay inside the buffer it was given, at every capacity".

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/detection.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;

    Detection det = {};
    // Derive the frame from the input, bounded the way the radio bounds it.
    const uint8_t profile = data[0] % 4;
    const size_t rawLen = (size - 1) > DETECTION_RAW_MAX_LEN ? DETECTION_RAW_MAX_LEN : (size - 1);
    det.profile = profile;
    det.off_grid = (data[0] & 0x80) != 0;
    if (!detectionSetRawPacket(det, data + 1, rawLen)) return 0;

    detectionApplyMeshtasticHeader(det, det.raw_packet, det.raw_len);
    (void)detectionClassification(det);
    (void)detectionProtocolCandidate(det);
    (void)detectionAuthStatus(det);
    (void)meshtasticIsBroadcast(det.raw_packet, det.raw_len);

    // Every capacity from zero to one byte past what a full row needs. A
    // formatter that writes its terminator one byte past a tight buffer only
    // shows up when the capacity is exactly wrong, which is how A05 hid.
    static uint8_t arena[DETECTION_CSV_MAX_ROW + 64];
    for (size_t cap = 0; cap <= DETECTION_CSV_MAX_ROW + 8; cap++) {
        memset(arena, 0xAA, sizeof(arena));
        char *out = (char *)arena;
        const size_t n = detectionFormatCsv(det, out, cap, "2026-09-07T00:00:00Z",
                                            (data[0] & 1) != 0, 45.5, -122.6, 1, 7);
        if (n == 0) {
            // Refusal must not have written anything reachable past capacity.
            continue;
        }
        if (n >= cap) __builtin_trap();          // must leave room for the NUL
        if (out[n] != '\0') __builtin_trap();    // must terminate where it says
        if (strlen(out) != n) __builtin_trap();  // returned length must be real
        for (size_t i = cap; i < sizeof(arena); i++) {
            if (arena[i] != 0xAA) __builtin_trap(); // wrote past the capacity
        }
    }
    return 0;
}
