#include "meshtastic_identity.h"

#include <mbedtls/aes.h>

namespace {

constexpr uint8_t DEFAULT_MESHTASTIC_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

} // namespace

bool meshtasticDecodeDefaultNodeIdentity(const Detection &det, NodeIdentity &out) {
    if (det.profile != (uint8_t)MissionProfile::MESHTASTIC ||
        det.raw_len <= MESHTASTIC_HEADER_LEN || det.raw_len > DETECTION_RAW_MAX_LEN ||
        det.node_id == 0 || det.packet_id == 0) return false;

    const size_t cipherLen = det.raw_len - MESHTASTIC_HEADER_LEN;
    uint8_t plaintext[DETECTION_RAW_MAX_LEN - MESHTASTIC_HEADER_LEN];
    uint8_t nonce[16] = {};
    uint8_t stream[16] = {};
    size_t offset = 0;
    memcpy(nonce, &det.packet_id, sizeof(det.packet_id));
    memcpy(nonce + 8, &det.node_id, sizeof(det.node_id));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const int keyStatus = mbedtls_aes_setkey_enc(&aes, DEFAULT_MESHTASTIC_PSK, 128);
    const int cryptStatus = keyStatus == 0
        ? mbedtls_aes_crypt_ctr(&aes, cipherLen, &offset, nonce, stream,
                                det.raw_packet + MESHTASTIC_HEADER_LEN, plaintext)
        : -1;
    mbedtls_aes_free(&aes);
    if (cryptStatus != 0) return false;
    return meshtasticParseNodeIdentityPayload(plaintext, cipherLen, det.node_id,
                                              det.rx_millis, det.raw_len, out);
}
