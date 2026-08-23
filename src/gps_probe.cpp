// LoRaTrace RX — standalone GPS bring-up probe.
//
// NOT part of the Phase 1 firmware and NOT the Phase 2 gps_task. This is a
// deliberately dumb, self-contained smoke test for one question: does the
// GPS module on this board actually talk to us on the pins board_pins.h
// claims, and does it eventually produce a fix?
//
// Why a separate build env rather than a few lines bolted into main.cpp:
// the 2026-08-23 sync-word bug is the argument. Radio RX went unverified
// for days because it was entangled with everything else booting; the
// lesson is to bring each piece of hardware up alone, where its failure
// mode is unambiguous. This also keeps the working RX firmware untouched —
// nothing here can perturb radio timing, because none of it is compiled
// into that build.
//
// Build/flash:  pio run -e gps-probe --target upload
// Return to the real firmware:  pio run -e cardputer-adv --target upload
//
// What to look for on serial at 115200:
//   * Raw NMEA sentences ($GPGGA / $GNGGA / $GPRMC / $GNRMC ...) within a
//     second or two of boot. If NOTHING appears, it's wiring/pins/baud,
//     not satellites — see the troubleshooting notes at the bottom.
//   * "sentences=N" climbing. Traffic proves the UART is right even with
//     zero satellites; a cold module indoors will happily emit empty
//     sentences for a long time.
//   * FIX ACQUIRED, once GGA reports a non-zero fix quality. Cold start
//     under open sky is typically minutes, and indoors may be never — a
//     window or outdoors is the honest test.

#include <Arduino.h>

#include "board_pins.h"
#include "version.h"

namespace {

HardwareSerial gps(1); // UART1; UART0 is the USB-CDC console

unsigned long sentenceCount = 0;
unsigned long lastReport = 0;
bool sawAnyByte = false;
bool sawFix = false;

// One NMEA sentence, accumulated without dynamic allocation (CLAUDE.md: no
// large heap buffers). NMEA sentences are capped at 82 chars by spec;
// oversize input is discarded rather than grown into.
char line[96];
size_t lineLen = 0;

// Pulls field `index` (0 = the "$GPGGA" tag itself) out of an NMEA sentence
// into `out`. Returns false if the field doesn't exist. Deliberately hand
// rolled: pulling in TinyGPS++ for a bring-up probe would mean the probe
// could fail because of a library, which defeats its purpose.
bool nmeaField(const char *s, uint8_t index, char *out, size_t outSize) {
    uint8_t field = 0;
    size_t w = 0;
    for (const char *p = s;; p++) {
        if (*p == ',' || *p == '\0' || *p == '*') {
            if (field == index) {
                out[w] = '\0';
                return true;
            }
            field++;
            w = 0;
            if (*p == '\0' || *p == '*') return false;
            continue;
        }
        if (field == index && w + 1 < outSize) out[w++] = *p;
    }
}

// GGA field 6 is fix quality: 0 = no fix, 1 = GPS, 2 = DGPS, ...
// RMC field 2 is status: 'A' = active/valid, 'V' = void.
void inspectSentence(const char *s) {
    char buf[16];
    const bool isGGA = (strstr(s, "GGA") != nullptr);
    const bool isRMC = (strstr(s, "RMC") != nullptr);

    if (isGGA && nmeaField(s, 6, buf, sizeof(buf)) && buf[0] != '\0' && buf[0] != '0') {
        char sats[16] = "?";
        nmeaField(s, 7, sats, sizeof(sats));
        if (!sawFix) {
            Serial.print(F("\n*** FIX ACQUIRED *** quality="));
            Serial.print(buf);
            Serial.print(F(" satellites="));
            Serial.println(sats);
            sawFix = true;
        }
    }
    if (isRMC && nmeaField(s, 2, buf, sizeof(buf)) && buf[0] == 'A' && !sawFix) {
        Serial.println(F("\n*** RMC reports a valid fix ***"));
        sawFix = true;
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000) {}

    Serial.print(F("LoRaTrace GPS probe v"));
    Serial.println(FIRMWARE_VERSION);
    Serial.print(F("UART1 RX=G"));
    Serial.print(PIN_GPS_RX);
    Serial.print(F(" TX=G"));
    Serial.print(PIN_GPS_TX);
    Serial.print(F(" baud="));
    Serial.println(GPS_BAUD);
    Serial.println(F("Raw NMEA follows. Silence here means wiring/baud, not satellites."));
    Serial.println(F("----------------------------------------------------------------"));

    gps.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    lastReport = millis();
}

void loop() {
    while (gps.available()) {
        char c = (char)gps.read();
        sawAnyByte = true;
        Serial.write(c); // raw passthrough — the primary evidence

        if (c == '\n' || c == '\r') {
            if (lineLen > 0) {
                line[lineLen] = '\0';
                sentenceCount++;
                inspectSentence(line);
                lineLen = 0;
            }
            continue;
        }
        if (lineLen + 1 < sizeof(line)) {
            line[lineLen++] = c;
        } else {
            lineLen = 0; // oversize/garbled — resync at the next newline
        }
    }

    // Periodic heartbeat so a silent module is obviously silent rather than
    // just looking like a quiet one.
    unsigned long now = millis();
    if (now - lastReport >= 5000) {
        lastReport = now;
        Serial.print(F("\n[probe] t="));
        Serial.print(now / 1000);
        Serial.print(F("s sentences="));
        Serial.print(sentenceCount);
        Serial.print(F(" fix="));
        Serial.println(sawFix ? F("YES") : F("no"));

        if (!sawAnyByte) {
            Serial.println(F("[probe] No bytes at all from the GPS UART. In order of likelihood:"));
            Serial.println(F("  1. RX/TX swapped — try PIN_GPS_RX/PIN_GPS_TX exchanged in board_pins.h"));
            Serial.println(F("  2. Wrong baud — many NMEA modules are 9600, not 115200; try GPS_BAUD=9600"));
            Serial.println(F("  3. Module unpowered or held in reset (check the Cap's own power/enable)"));
            Serial.println(F("  Satellites are NOT a candidate: an unlocked module still emits empty sentences."));
        } else if (!sawFix) {
            Serial.println(F("[probe] UART is good (bytes arriving). No fix yet — that's antenna/sky,"));
            Serial.println(F("        not wiring. Cold start outdoors is typically minutes; indoors may never."));
        }
    }
}
