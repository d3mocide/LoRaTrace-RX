# Heltec standalone sweep — research note

**Status:** implemented and hardware-verified for the USB/serial path
(2026-08-28); PRG-button path implemented but physically unverified. Bench
exploration, not a Phase 8/9 exit-gate item.

## Context

This session's Cardputer was disconnected for a solo bench session, leaving
only the Heltec V4R8 bench transmitter (`bench/heltec-v4r8-transmitter/`)
attached. The prompt: with real firmware already on this device and a whole
session to experiment, what else can it usefully do on its own, and can any
of the existing Phase 8/9 test ideas be triggered directly from the device
rather than only from a host script?

The Heltec firmware up to this point was purely reactive: it holds one of
nine compiled Meshtastic candidate tuples, and a host sends `HELLO` /
`STATUS` / `CONFIG` / `ARM` / `QUIET` over a CRC-framed USB protocol to fire
one capped (-9dBm) tagged packet at a host-relative deadline. It has no
mode that runs anything on its own, and no on-device trigger at all — every
prior use required a host script and, implicitly, a Cardputer to receive
the pulse.

## Ideas considered

1. **Standalone RSSI/energy sweep, parameter-matched to the Cardputer's
   `ENERGY_SWEEP`.** Chosen — see below.
2. **Autonomous "fire every candidate" TX cycle** (button press walks
   through all nine candidates, arming one pulse each). Would exercise the
   TX side end to end without a host, but produces nothing new to look at
   without a Cardputer already listening — it's a rerun of what `ARM` does
   nine times, not a new capability. Deferred; trivial to add later if a
   walk-through-all-candidates bench step turns out to be wanted.
3. **Field/walking beacon mode** (periodic TX while carried around, to
   range-test the Cardputer's detection UI outdoors). Rejected outright:
   the existing README is explicit that "the fixture is closed and
   low-power... not an over-the-air field test transmitter"
   (`research/phase8-low-profile-harness-design.md`). Building a mode whose
   only purpose is untethered outdoor transmission works directly against
   that documented boundary, so I didn't build it even experimentally.
4. **On-device power/calibration self-test** (verify actual radiated power
   against the -9dBm setting). Not attempted — needs a spectrum analyzer or
   calibrated power meter, neither of which is bench equipment here.

Idea 1 won because it turns the Heltec from a disposable TX pulse prop into
a second, independent measurement instrument, it's pure RX so it can't run
into the field-transmitter boundary in #3, and it directly speaks to a
standing gap called out in `PROGRESS.md`'s Phase 9 section: the sweep
margin calibration bench "cannot produce a known-quiet RF control." A
second physically separate SX1262, sitting in the same room, sampling the
same band with the same math, is a real (if partial) way to cross-check
whether a given room's reading is one radio's artifact or genuinely
ambient — without waiting for the Cardputer to be back on the bench.

## What was built

`bench/heltec-v4r8-transmitter/src/main.cpp` gained:

- A new `SWEEP` command over the existing `@LTTX/1` framed protocol.
- A `pollSweepButton()` no-host trigger: hold the PRG button (GPIO0) for
  2 seconds.
- A `STATUS` response extension: trailing `;SW=<peaks>/<total>`.

The sweep itself is deliberately copied, not reinvented, from the
Cardputer's own Pass-A engine so the two devices' numbers are comparable:

| Parameter | Value | Source |
|---|---|---|
| Band | 868.000–923.000MHz | `src/energy_plan.h` |
| Step / bin count | 250kHz / 221 bins | `src/energy_plan.h` |
| Modem params | SF8 / BW125 / CR4:5 / sync 0x2B (MESH_OREGON) | `src/channel_plans.h`'s home default |
| Samples/bin | 4, via `getRSSI(false)` (instantaneous) | `radio_task.cpp::performEnergySweep()` |
| Sample interval | 1ms | `radio_task.cpp` `ENERGY_SAMPLE_INTERVAL_MS` |
| Noise floor | EMA, divisor 8, seeded from bin 0 | `src/energy_observation.h` |
| Peak margin | 35.0dB (350 in tenths-of-dBm) | `src/energy_observation.h`'s calibrated default |
| dBm→fixed rounding | round-half-away-from-zero, same formula | `energyRssiDbmToFixed()` |

**On "hardcoded to MESH_OREGON" — what that does and doesn't mean**
(clarified 2026-08-28 after an operator question about whether this makes
Sweep bound to a profile). Two separate things are easy to conflate:

- **The frequency range swept is not tied to any profile or candidate at
  all.** It's a fixed, uniform 868.000–923.000MHz/221-bin grid on both
  devices, regardless of Meshtastic/MeshCore/anything else —
  `energy_plan.h`'s own comment: "Sweep's bins are one uniform grid,
  shared by RETICULUM and GENERAL_EXPLORATION alike."
- **What *is* fixed is the modem config used while sampling RSSI at each of
  those 221 frequencies** — and Sweep never demodulates or decodes
  anything (`getRSSI(false)` is a raw instantaneous reading, not a packet
  receive), so the sync word and coding rate in that config are inert
  bookkeeping; they don't gate what counts as "activity." Only bandwidth
  (125kHz) meaningfully affects the reading, since it sets the receiver's
  analog filter width per sample. So in effect Sweep is already close to
  protocol-agnostic — it reads elevated RSSI regardless of what's
  transmitting there.
- MESH_OREGON's SF/BW/CR/sync gets used only because RadioLib's `begin()`
  requires a complete modem config for any RSSI read, and reusing the home
  channel's existing config was a documented shortcut to ship this
  quickly, not a permanent design choice — `radio_task.cpp`'s own comment:
  "Sweep is protocol-agnostic energy measurement, not a decode attempt —
  reusing the home channel's own SF/BW/CR/sync keeps this slice simple; a
  dedicated wide-BW scan config is a future calibration decision, not a
  correctness requirement." `PROGRESS.md`'s Phase 9 checklist still
  carries this open: "Two-pass acquisition: bounded energy statistics
  first, then LoRa CAD only at measured peaks... Pass A only so far." This
  Heltec tool copies the same MESH_OREGON tuple specifically so its Pass-A
  numbers stay comparable to the Cardputer's during this still-open
  characterization work — not because that's been decided as the final
  scan config.

One deliberate divergence: the Cardputer only logs *peak* bins (sparse, by
design — DESIGN.md §8.1). This standalone tool prints every bin
(`#SWEEP <bin> <freq_khz> <avg_dbm_x10> <peak_dbm_x10> <is_peak> <ok>`) as
plain unframed serial text, because there's no SD card or storage budget
argument here, and full-resolution output is more useful for a
side-by-side comparison than a sparse one. These lines are ordinary serial
output, not `@LTTX/1` protocol traffic, matching the same "human diagnostic
lines stay ordinary lines" convention the Cardputer's own Serial Control
design already uses.

`SWEEP` refuses to run while a pulse is armed (`ERROR ARMED`) so it can
never push a host-scheduled `ARM` deadline out — the whole thing is
synchronous/blocking (no FreeRTOS tasks in this firmware), so a sweep
mid-countdown would otherwise delay the actual transmission. It restores
the previously-configured TX candidate's modem config when done, so the
bench transmitter's normal `ARM` behavior is unaffected afterward.

## Hardware verification (real device, `/dev/ttyACM0`)

Built with `pio run -e heltec-v4r8` (clean compile, RAM 6.6%/Flash 5.0%),
flashed to the attached board, then driven over the real framed protocol
using PlatformIO's bundled Python (`~/.platformio/penv/bin/python3`, which
already has `pyserial` — the system Python does not).

```text
@LTTX/1 1 ACK V4R8;SX1262;MAXM9 C2EC
@LTTX/1 2 STATUS C=LONG_FAST;A=0;SW=NONE 8D3C
@LTTX/1 3 ACK MESH_OREGON 055A
@LTTX/1 4 STATUS C=MESH_OREGON;A=0;SW=NONE C159
--- SWEEP ---
@LTTX/1 5 ACK SWEEP_STARTED BC5D
#SWEEP START 868000 923000 221
#SWEEP 0 868000 -1153 -1080 0 1
#SWEEP 1 868250 -1190 -1070 0 1
...
#SWEEP 220 923000 -1200 -1100 0 1
@LTTX/1 5 SWEEP_DONE P=0/221 8E5A
```

Full 221-bin sweep completed in 9.7 seconds. 0/221 peaks against a
~-119 to -120dBm average floor with per-bin peaks around -108 to -113dBm —
a plausible quiet-office reading, no anomalies, no reboot (confirmed by
`lastSweepCounts` correctly surviving into the next `STATUS` call, which
would reset to `NONE` across a power cycle).

Follow-up checks:

- `STATUS` after the sweep: `SW=0/221` — the new field round-trips
  correctly.
- Guard behavior: `ARM 500` accepted, then `SWEEP` immediately after
  returned `ERROR ARMED` (rejected, as designed), and the armed pulse still
  fired on schedule (`TX_STARTED` / `TX_DONE OK`) — the guard doesn't
  interfere with the pulse it's protecting.
- Malformed frame: a deliberately wrong CRC (`@LTTX/1 103 STATUS - FFFF`)
  produced zero reply bytes — matches the existing "reject silently"
  security posture, unchanged by this work.

## Open items

- **PRG-button trigger is unverified.** I don't have a way to physically
  press a button on hardware; the code path (`pollSweepButton()`,
  active-low on GPIO0 with `INPUT_PULLUP`, 2-second hold, re-arm only after
  release) is a standard ESP32/Heltec PRG-button read, and no unexpected
  sweep fired during ~15+ seconds of unrelated serial testing (which it
  would have, immediately and loudly, if the pin were stuck low) — but
  someone needs to actually hold the button once to confirm.
- **No Cardputer to cross-check against.** The whole point of parameter-
  matching this to `ENERGY_SWEEP` is so a Heltec sweep and a Cardputer
  sweep, taken back-to-back in the same room, can be compared bin-for-bin.
  That comparison hasn't happened yet — it needs both devices on the bench
  at once.
- **No long-soak watchdog check.** One sweep (9.7s, ~221 `radio.begin()`
  calls) completed cleanly with no reset. Repeated back-to-back sweeps
  haven't been stress-run the way Phase 8's 1,000-cycle harness stress-ran
  `ARM`.
- **`src/version.h` was not bumped.** That versioning discipline
  (CLAUDE.md) covers the Cardputer RX firmware's phase progression; this is
  bench-only tooling in a separate PlatformIO project with its own
  lifecycle, so it wasn't touched.
