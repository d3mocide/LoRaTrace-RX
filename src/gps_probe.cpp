// LoRaTrace RX — standalone GPS bring-up probe.
//
// NOT part of the Phase 1/2 firmware. This is a deliberately dumb,
// self-contained smoke test for one question: does the GPS module actually
// talk to us, and does it eventually produce a fix?
//
// Build/flash:  pio run -e gps-probe --target upload
// Return to the real firmware:  pio run -e cardputer-adv --target upload
//
// History worth keeping, because it shaped this file (PROGRESS.md
// 2026-08-23): the first version isolated itself from the rest of the boot
// sequence so its failure mode would be unambiguous — and thereby skipped
// the IO-expander init, which is what *powers the GPS*. It reported zero
// bytes and blamed wiring. Two lessons are baked in below:
//
//   * Power and clocks are not variables worth isolating. Bring them up.
//   * When two sources disagree about a pin, don't pick one and ask the
//     operator to reflash if it's wrong — test both and report which works.
//     M5Stack's docs table and their own example code contradict each other
//     on GPS RX/TX polarity (see board_pins.h), so this probe A/Bs them.
//
// What to look for on serial at 115200:
//   * "IO expander: OK" — the GPS is powered. If this fails, nothing else
//     below means anything.
//   * Raw NMEA ($GPGGA / $GNGGA / $GPRMC / $GNRMC ...) within a second or
//     two, and a "WORKING PIN ORDER" banner naming which mapping produced
//     it. Put that mapping in board_pins.h.
//   * "sentences=N" climbing. Traffic proves the UART even with zero
//     satellites; a cold module indoors emits empty sentences for a long
//     time.
//   * FIX ACQUIRED, once a fix is reported. Cold start under open sky is
//     typically minutes; indoors may be never.

#include <Arduino.h>

#include "board_pins.h"
#include "gps_parse.h"
#include "io_expander.h"
#include "nmea.h"
#include "version.h"

namespace {

HardwareSerial gps(1); // UART1; UART0 is the USB-CDC console

// How long to listen on one pin ordering before trying the other. Long
// enough that a slow-starting module isn't mistaken for a wrong pinout.
constexpr uint32_t PIN_TRIAL_MS = 8000;

struct PinOrder {
    int8_t rx;
    int8_t tx;
    const char *label;
};

// Primary first: M5Stack's own working example code says the ESP32 receives
// on G15. The alternate is their docs table's reading, already shown to
// produce silence on 2026-08-23 — kept so the probe proves it rather than
// assuming it.
const PinOrder PIN_ORDERS[] = {
    {PIN_GPS_RX, PIN_GPS_TX, "RX=G15 TX=G13 (M5Stack example code)"},
    {PIN_GPS_RX_ALT, PIN_GPS_TX_ALT, "RX=G13 TX=G15 (M5Stack docs table)"},
};
constexpr size_t PIN_ORDER_COUNT = sizeof(PIN_ORDERS) / sizeof(PIN_ORDERS[0]);

size_t activeOrder = 0;
uint32_t orderStartedMs = 0;
bool orderLatched = false; // stop switching once bytes arrive

unsigned long sentenceCount = 0;
unsigned long badChecksumCount = 0;
uint32_t lastReport = 0;
uint32_t firstByteMs = 0;
bool sawAnyByte = false;
bool sawFix = false;
bool expanderOk = false;

GpsFix fix;

char line[NMEA_MAX_SENTENCE];
size_t lineLen = 0;

// Raw NMEA is a firehose once the module is talking (~80 sentences per 5s
// across five constellations), which buries the one line that matters. Dump
// it only briefly as proof-of-life, then switch to the summary.
constexpr uint32_t RAW_DUMP_MS = 3000;

// Satellite visibility and fix type now live in gps_parse.h's GpsFix, which
// the Phase 2 GPS task uses too — this probe deliberately carries no private
// copy of that logic. Beyond avoiding duplication, it means the probe
// exercises the exact parser the firmware depends on, so a bug found here is
// a bug found there.

// Last $GxTXT payload — the module's own words about its health.
char lastTxt[48] = {0};

void startOrder(size_t idx) {
    activeOrder = idx;
    orderStartedMs = millis();
    gps.end();
    gps.begin(GPS_BAUD, SERIAL_8N1, PIN_ORDERS[idx].rx, PIN_ORDERS[idx].tx);
    Serial.print(F("\n[probe] Listening with "));
    Serial.println(PIN_ORDERS[idx].label);
}

void handleSentence(const char *s) {
    // gpsApplySentence rejects bad checksums, so count them separately —
    // a stream of checksum failures means the UART is *nearly* right
    // (plausible baud, noisy line) rather than wrong, which is a very
    // different diagnosis from silence.
    if (!nmeaChecksumValid(s)) {
        badChecksumCount++;
        return;
    }
    sentenceCount++;
    gpsApplySentence(fix, s, millis());

    // GSV/GSA are handled inside gpsApplySentence() above; only TXT is
    // probe-specific (the firmware has no use for it, but a bring-up tool
    // very much does).
    char tag[8];
    if (!nmeaField(s, 0, tag, sizeof(tag)) || strlen(tag) < 3) return;

    if (strstr(tag, "TXT")) {
        // The module reporting on itself. Surfaced prominently the first
        // time and whenever it changes, rather than scrolling past in the
        // raw dump — "ANTENNA OPEN" is exactly the kind of line that
        // matters and is trivially missed.
        // Its own buffer, not the 16-byte `buf`: module TXT messages can be
        // longer than a numeric field and truncating one would make it
        // harder to recognise, which defeats the point of surfacing it.
        char txt[sizeof(lastTxt)];
        if (nmeaField(s, 4, txt, sizeof(txt)) && strcmp(lastTxt, txt) != 0) {
            strncpy(lastTxt, txt, sizeof(lastTxt) - 1);
            lastTxt[sizeof(lastTxt) - 1] = '\0';
            Serial.print(F("\n[probe] Module says: \""));
            Serial.print(lastTxt);
            Serial.println(F("\""));
            if (strstr(lastTxt, "ANTENNA OPEN") != nullptr) {
                Serial.println(F("        NOTE: this Cap has a built-in PASSIVE ceramic antenna,"));
                Serial.println(F("        which draws no bias current — so the module's antenna"));
                Serial.println(F("        supervisor reports OPEN even when all is well. Treat as"));
                Serial.println(F("        benign unless satellites-in-view stays 0 under open sky."));
            }
        }
    }

    if (!sawFix && fix.has_position) {
        Serial.println(F("\n*** FIX ACQUIRED ***"));
        Serial.print(F("  lat="));
        Serial.print(fix.lat, 6);
        Serial.print(F(" lon="));
        Serial.print(fix.lon, 6);
        Serial.print(F(" quality="));
        Serial.print(fix.fix_quality);
        Serial.print(F(" sats="));
        Serial.println(fix.satellites);
        sawFix = true;
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) {}

    Serial.print(F("LoRaTrace GPS probe v"));
    Serial.println(FIRMWARE_VERSION);

    // THE thing the first version of this probe got wrong. P0 on the
    // PI4IOE5V6408 powers the GPS as well as switching the RF antenna path,
    // so without this the module is simply off and every UART reading below
    // is meaningless.
    expanderOk = ioExpanderInit();
    if (expanderOk) {
        Serial.println(F("IO expander: OK — P0 high (GPS powered, antenna switch enabled)."));
    } else {
        Serial.println(F("IO expander: FAILED — no I2C ACK at 0x43."));
        Serial.println(F("  The GPS is almost certainly unpowered; UART silence below is expected"));
        Serial.println(F("  and is NOT evidence about the pin mapping. Fix this first."));
    }

    Serial.print(F("Baud "));
    Serial.print(GPS_BAUD);
    Serial.println(F(" 8N1. Will A/B both documented pin orders until bytes appear."));
    Serial.println(F("----------------------------------------------------------------"));

    startOrder(0);
    lastReport = millis();
}

void loop() {
    while (gps.available()) {
        char c = (char)gps.read();
        if (!sawAnyByte) {
            sawAnyByte = true;
            orderLatched = true;
            firstByteMs = millis();
            Serial.print(F("\n*** WORKING PIN ORDER: "));
            Serial.print(PIN_ORDERS[activeOrder].label);
            Serial.println(F(" ***"));
            Serial.println(F("    Put this mapping in board_pins.h if it isn't already."));
            Serial.println(F("    Raw NMEA for a few seconds as proof of life, then summary only."));
        }
        // Raw passthrough is proof of life, not a monitoring mode: at ~80
        // sentences per 5s it buries everything useful. Time-boxed.
        if (millis() - firstByteMs < RAW_DUMP_MS) Serial.write(c);

        if (c == '\n' || c == '\r') {
            if (lineLen > 0) {
                line[lineLen] = '\0';
                handleSentence(line);
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

    uint32_t now = millis();

    // Rotate pin orderings until something speaks.
    if (!orderLatched && PIN_ORDER_COUNT > 1 && (now - orderStartedMs) >= PIN_TRIAL_MS) {
        startOrder((activeOrder + 1) % PIN_ORDER_COUNT);
    }

    if (now - lastReport >= 5000) {
        lastReport = now;
        Serial.print(F("\n[probe] t="));
        Serial.print(now / 1000);
        Serial.print(F("s sentences="));
        Serial.print(sentenceCount);
        Serial.print(F(" badcrc="));
        Serial.print(badChecksumCount);
        Serial.print(F(" fixtype="));
        Serial.print(fix.fix_type); // 1=none 2=2D 3=3D
        Serial.print(F(" sats="));
        Serial.print(fix.sats_in_view);
        if (fix.talker_count > 0) {
            Serial.print(F(" ("));
            for (uint8_t i = 0; i < fix.talker_count; i++) {
                if (i) Serial.print(' ');
                Serial.print(fix.talkers[i].id);
                Serial.print(':');
                Serial.print(fix.talkers[i].in_view);
            }
            Serial.print(')');
        }
        Serial.print(F(" fix="));
        Serial.println(sawFix ? F("YES") : F("no"));

        if (!sawAnyByte) {
            if (!expanderOk) {
                Serial.println(F("[probe] Still no bytes — but the IO expander failed, so the GPS"));
                Serial.println(F("        is unpowered. Chase the I2C failure, not the UART."));
            } else {
                Serial.println(F("[probe] No bytes on either pin order yet. Remaining suspects:"));
                Serial.println(F("  1. Baud — 115200 is documented, but some modules ship at 9600."));
                Serial.println(F("     Try GPS_BAUD=9600 in board_pins.h."));
                Serial.println(F("  2. Cap not fully seated (the GPS shares the same connector)."));
                Serial.println(F("  3. Module held in reset / a dead ceramic antenna feed."));
                Serial.println(F("  Satellites are NOT a candidate: an unlocked module still emits"));
                Serial.println(F("  empty sentences continuously."));
            }
        } else if (badChecksumCount > 0 && sentenceCount == 0) {
            Serial.println(F("[probe] Bytes arriving but every sentence fails checksum — that's a"));
            Serial.println(F("        baud mismatch or a noisy line, not a wiring fault."));
        } else if (!sawFix) {
            // With the UART proven, the useful split is whether the antenna
            // can SEE anything. Zero in view everywhere is a sky/antenna
            // problem; satellites in view without a fix is just patience.
            uint16_t inView = fix.sats_in_view;
            if (inView == 0) {
                Serial.println(F("[probe] UART good, but 0 satellites IN VIEW on every constellation."));
                Serial.println(F("        That is normal indoors — GPS is ~-130dBm and a roof costs"));
                Serial.println(F("        20-30dB. Take it outside with a clear view of open sky."));
                Serial.println(F("        If it stays 0 outdoors for several minutes, then suspect"));
                Serial.println(F("        the antenna for real."));
            } else {
                Serial.println(F("[probe] Satellites in view but no fix yet — the antenna is working"));
                Serial.println(F("        and it just needs time to download almanac/ephemeris."));
                Serial.println(F("        A cold start can take several minutes. Keep it still."));
            }
        }
    }
}
