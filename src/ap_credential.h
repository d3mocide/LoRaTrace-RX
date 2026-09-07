#pragma once
// LoRaTrace RX — the AP's WPA2 pre-shared key.
//
// A single hardcoded password shipped in the firmware is a password every
// holder of any LoRaTrace build already knows, and this AP serves raw
// frames, decoded identities and a GPS track (audit A10). The key is
// therefore per device: generated from the hardware RNG on first use and
// persisted, so it survives reboots and an operator can write it down once.
//
// Rotation is deliberately "delete the file": there is no on-device text
// entry, and a menu row that silently replaced a working key on a stray
// keypress would strand an operator mid-drive. See SECURITY.md.
//
// The validation and generation halves are pure so they can be host-tested;
// the SD half lives in the .cpp.

#include <stddef.h>
#include <stdint.h>

// WPA2-PSK accepts 8..63 ASCII characters. 12 from a 32-symbol alphabet is
// 60 bits, which is far past anything worth attacking a field AP for, and
// still short enough to type from the device's own screen.
constexpr size_t AP_KEY_LEN = 12;
constexpr size_t AP_KEY_BUF = AP_KEY_LEN + 1;

// No 0/O/1/l/i: the operator reads this off a 240x135 screen and types it
// into a phone. Exactly 32 symbols, so a 5-bit draw needs no rejection loop
// and every symbol stays equally likely.
constexpr const char *AP_KEY_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789+";

inline bool apKeyCharAllowed(char c) {
    for (const char *p = AP_KEY_ALPHABET; *p != '\0'; ++p) {
        if (*p == c) return true;
    }
    return false;
}

// A key we would have written ourselves. Anything else on the card is
// treated as damage and replaced rather than used: a short or mangled line
// would otherwise silently weaken the AP.
inline bool apKeyIsValid(const char *key) {
    if (key == nullptr) return false;
    size_t len = 0;
    for (; key[len] != '\0'; ++len) {
        if (len >= AP_KEY_LEN || !apKeyCharAllowed(key[len])) return false;
    }
    return len == AP_KEY_LEN;
}

// `randomByte` supplies uniform bytes; only the low 5 bits of each are used.
inline void apKeyGenerate(char *out, size_t outSize, uint8_t (*randomByte)()) {
    if (out == nullptr || outSize < AP_KEY_BUF || randomByte == nullptr) return;
    for (size_t i = 0; i < AP_KEY_LEN; ++i) out[i] = AP_KEY_ALPHABET[randomByte() & 0x1F];
    out[AP_KEY_LEN] = '\0';
}

// Loads the persisted key, generating and saving one on first use. Returns
// false when the key could not be persisted — the caller still gets a usable
// key for this session, but it will differ after a reboot, and an operator
// who wrote the old one down needs to be told.
bool apKeyLoad(char *out, size_t outSize);

// The key in use this session; empty before apKeyLoad(). Never logged, never
// exported, never returned over HTTP.
const char *apKeyCurrent();
