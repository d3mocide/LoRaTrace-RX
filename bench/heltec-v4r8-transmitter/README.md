# Heltec V4 R8 deterministic transmitter

Bench-only firmware for the operator-confirmed Heltec WiFi LoRa 32 V4 R8
(ESP32-S3R8 + SX1262). It replaces Meshtastic while flashed; restore the
Meshtastic image before returning the node to normal use.

Use only with the Phase 8 attenuated/coax or shielded fixture, **except**
`BEACON` mode (below), which the operator explicitly authorized as an
over-the-air field-test exception for Cardputer range/detection
verification — see the amendment note in
`research/phase8-low-profile-harness-design.md` and the full design in
`research/heltec-menu-and-beacon.md`. The firmware caps the SX1262 output
setting at -9 dBm and accepts a maximum five-second arm delay; neither
limit substitutes for correct attenuation or local RF rules.

The USB protocol is line/CRC-framed:

```text
@LTTX/1 <sequence> <command> <argument> <crc16>\n
@LTTX/1 1 HELLO - <crc16>
@LTTX/1 2 CONFIG LONG_MODERATE <crc16>
@LTTX/1 3 ARM 250 <crc16>
```

Commands are `HELLO`, `STATUS`, `CONFIG`, `ARM`, `QUIET`, `SWEEP`, and
`BEACON`.
`SWEEP` runs a standalone RSSI energy scan on this device's own SX1262,
parameter-matched to the Cardputer's `ENERGY_SWEEP` (same 868.000-923.000MHz
band, 250kHz/221-bin grid, MESH_OREGON modem params, 4 samples/bin, 35.0dB
margin over an EMA-8 rolling floor — see `src/energy_plan.h`,
`src/energy_observation.h`, and `radio_task.cpp`'s `performEnergySweep()` in
the main repo) so a sweep taken here is directly comparable to one taken on
the Cardputer in the same room, not a separate experiment with its own
numbers. It refuses to run while a pulse is armed (`ERROR ARMED`) so it can
never delay a host-scheduled `ARM` deadline. Every bin is also printed as an
unframed `#SWEEP <bin> <freq_khz> <avg_dbm_x10> <peak_dbm_x10> <is_peak> <ok>`
line — ordinary serial output, not `@LTTX/1` protocol traffic, so a plain
terminal capture is enough to read full-resolution results without any host
script. Restores the previously-armed TX candidate's modem config when done.

`BEACON START`/`BEACON STOP` run a periodic capped-count TX pulse train
(2s interval, 120-pulse/~4-minute cap, same -9dBm as every other mode) at
the active candidate — the field/range-test exception noted above. It
refuses to start while a pulse is armed (`ERROR ARMED`) and `ARM`/`SWEEP`
both refuse while it's running (`ERROR BEACON_ACTIVE`); `QUIET` stops it
along with any armed pulse. Each pulse is logged as an unframed
`#BEACON <n> <candidate> <freq_khz> <OK|ERROR>` line.

`STATUS` gains trailing `;SW=<peaks>/<total>` and `;B=<0|1>` fields
reporting the last sweep result and whether a beacon is currently running.

`CONFIG`
accepts only the fixed Meshtastic candidates compiled into the image,
including the operator-requested physical `MESH_OREGON` fixture tuple
(918.5 MHz, SF8, BW125, CR4/5). It does not store channel names or PSKs.
`ARM` takes a delay in milliseconds and transmits one tagged packet; the
device reports `TX_STARTED` and `TX_DONE` using the command's sequence. A
host retries only with the same sequence after a lost response.

The built-in OLED and PRG button (GPIO0) form a Meshtastic-style single-
button menu: a short press/release cycles through five screens (`HOME`,
`CANDIDATE`, `FIRE`, `SWEEP`, `BEACON`; the `SWEEP` screen's own display
always reads `Fixed: MESH_OREGON`, never the selected TX candidate — Sweep
does not use `CANDIDATE`'s selection, see above), and holding the button
for 2.5 seconds acts on whichever screen is showing — advance the
candidate, fire one pulse, run a sweep, or start/stop the beacon. Holding
through 5 seconds, from any screen, is a second tier: an immediate
"stop everything" (cancels an armed pulse, stops the beacon if running,
the same effect as sending `QUIET`) — added once `BEACON` meant this
device could be transmitting untethered, so a fast, unambiguous physical
stop matters. Both hold thresholds fire independently during one
continuous hold — e.g. holding through 5s on the `BEACON` screen starts
the beacon at 2.5s and immediately stops it again at 5s. Every action
enforces the same guards its serial-command equivalent does (an armed
pulse or active beacon blocks the others), so the button can't reach a
state a host command couldn't also reach. The R8's white LED is on
whenever a pulse is armed or the beacon is running. Boot shows a brief
splash screen before the menu appears. This on-device path was added
2026-08-28 and the button hold-to-act behavior is now physically
confirmed working on real hardware; the 2.5s/5s two-tier timing and the
splash are not yet physically re-confirmed — see
`research/heltec-menu-and-beacon.md` for the design and what remains
unverified.

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
