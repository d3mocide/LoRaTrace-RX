# Heltec V4 R8 deterministic transmitter

Bench-only firmware for the operator-confirmed Heltec WiFi LoRa 32 V4 R8
(ESP32-S3R8 + SX1262). It replaces Meshtastic while flashed; restore the
Meshtastic image before returning the node to normal use.

Use only with the Phase 8 attenuated/coax or shielded fixture. The firmware
caps the SX1262 output setting at -9 dBm and accepts a maximum five-second
arm delay; neither limit substitutes for correct attenuation or local RF
rules.

The USB protocol is line/CRC-framed:

```text
@LTTX/1 <sequence> <command> <argument> <crc16>\n
@LTTX/1 1 HELLO - <crc16>
@LTTX/1 2 CONFIG LONG_MODERATE <crc16>
@LTTX/1 3 ARM 250 <crc16>
```

Commands are `HELLO`, `STATUS`, `CONFIG`, `ARM`, and `QUIET`. `CONFIG`
accepts only the source-backed Meshtastic candidates compiled into the image.
`ARM` takes a delay in milliseconds and transmits one tagged packet; the
device reports `TX_STARTED` and `TX_DONE` using the command's sequence. A
host retries only with the same sequence after a lost response.

The built-in OLED is a local bench-status display: it shows radio readiness,
the selected candidate, whether a packet is armed, and the last result. The
R8's white LED is on while an `ARM` request is pending. It has no on-device
transmit control; USB remains the only way to arm a pulse.

The first host-side loop is `scripts/phase8_bench.py`. It requires explicit
Cardputer and Heltec serial ports, captures every control line, and currently
exercises the LongModerate Probe candidate while the Cardputer is on its
built-in LongFast home tuple. It is a controlled smoke/matrix foundation, not
yet evidence for the Phase 8 cancellation/fault-injection exit criterion.

Build from this directory:

```text
pio run -e heltec-v4r8
pio run -e heltec-v4r8 --target upload
```

The V4 R8 uses SX1262 SPI NSS/SCK/MOSI/MISO on GPIO8/9/10/11, reset/busy/DIO1
on GPIO12/13/14, and its changed PA control on GPIO5. The board front-end is
enabled before radio initialization with GPIO7 and GPIO2 as documented by
Heltec; validate the first transmitted pulse through the attenuated fixture
before running a matrix.
