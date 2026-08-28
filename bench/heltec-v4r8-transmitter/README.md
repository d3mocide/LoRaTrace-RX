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
accepts only the fixed Meshtastic candidates compiled into the image,
including the operator-requested physical `MESH_OREGON` fixture tuple
(918.5 MHz, SF8, BW125, CR4/5). It does not store channel names or PSKs.
`ARM` takes a delay in milliseconds and transmits one tagged packet; the
device reports `TX_STARTED` and `TX_DONE` using the command's sequence. A
host retries only with the same sequence after a lost response.

The built-in OLED is a local bench-status display: it shows radio readiness,
the selected candidate, whether a packet is armed, and the last result. The
R8's white LED is on while an `ARM` request is pending. It has no on-device
transmit control; USB remains the only way to arm a pulse.

The shared host transport is `scripts/bench_harness.py`; the nominal scenario
is `scripts/phase8_bench.py`. The scenario requires explicit Cardputer and
Heltec serial ports, captures every control line, and exercises the
LongModerate Probe candidate while synchronizing on its reported frequency,
so a Cardputer home-channel override is allowed as long as it is not the
LongModerate target. `scripts/phase8_cancel_bench.py`,
`scripts/phase8_fault_bench.py`, and `scripts/phase8_contention_bench.py`
reuse the shared transport for cancellation, bench-only fault hooks, and USB
contention; future bench-node scenarios should follow that pattern rather
than copy this loop.

`scripts/phase8_cad_rate_bench.py` compares the Cardputer SX1262 CAD window
at 1, 2, 4, 8, and 16 symbols against the same LongModerate fixture. It
requires the dedicated `cardputer-adv-bench` image: its bounded `BENCH_CAD`
selector accepts only those five values, while a production image rejects the
command. A strict false-positive/miss-rate result also requires an attenuated
or shielded quiet control; an exposed-room run is retained as a room-rate
observation, not a calibration result.

For a receive-path diagnostic, the observe-only rate harness also accepts
`PULSE_CYCLES=0`; it holds the Heltec quiet and records quiet candidate activity
without ever claiming a gate result.

The launcher also writes a structured `.results.jsonl` event stream alongside
the human console log and raw serial capture.

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
