#include "ap_credential.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_random.h>

#include "config_line.h"
#include "file_transaction.h"
#include "spi_bus.h"

namespace {

constexpr const char *AP_KEY_PATH = "/loratrace/wifi.txt";
constexpr const char *AP_KEY_DIR = "/loratrace";

char currentKey[AP_KEY_BUF] = {0};

uint8_t hardwareRandomByte() {
    return (uint8_t)(esp_random() & 0xFF);
}

bool readKeyLocked(char *out, size_t outSize) {
    recoverTextFile(SD, AP_KEY_PATH);
    File f = SD.open(AP_KEY_PATH);
    if (!f) return false;
    bool found = false;
    while (f.available() && !found) {
        char line[64];
        if (!readBoundedLine(f, line, sizeof(line))) continue;
        char key[16], value[48];
        if (!configLineSplit(line, key, sizeof(key), value, sizeof(value))) continue;
        if (strcmp(key, "password") != 0 || !apKeyIsValid(value)) continue;
        strncpy(out, value, outSize - 1);
        out[outSize - 1] = '\0';
        found = true;
    }
    f.close();
    return found;
}

} // namespace

bool apKeyLoad(char *out, size_t outSize) {
    if (out == nullptr || outSize < AP_KEY_BUF) return false;

    // Generated before the card is consulted so a mount failure still yields
    // a real key rather than a predictable fallback.
    apKeyGenerate(currentKey, sizeof(currentKey), hardwareRandomByte);

    bool persisted = false;
    {
        SpiBusLock lock(pdMS_TO_TICKS(2000));
        if (lock.held()) {
            char stored[AP_KEY_BUF] = {0};
            if (readKeyLocked(stored, sizeof(stored))) {
                strncpy(currentKey, stored, sizeof(currentKey) - 1);
                currentKey[sizeof(currentKey) - 1] = '\0';
                persisted = true;
            } else {
                if (!SD.exists(AP_KEY_DIR)) SD.mkdir(AP_KEY_DIR);
                char text[48];
                const int n = snprintf(text, sizeof(text), "password=%s\n", currentKey);
                persisted = n > 0 && (size_t)n < sizeof(text) &&
                            replaceTextFile(SD, AP_KEY_PATH, text, (size_t)n);
            }
        }
    }

    strncpy(out, currentKey, outSize - 1);
    out[outSize - 1] = '\0';
    return persisted;
}

const char *apKeyCurrent() {
    return currentKey;
}
