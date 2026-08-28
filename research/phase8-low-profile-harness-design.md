# Phase 8 bench harness and Low Profile control design

**Status:** proposed implementation plan, 2026-08-27.

This design has two deliberately separate consumers of one bounded command
core:

1. a trusted, wired Phase 8 lab harness that proves Probe's hardware exit
   criteria; and
2. **Low Profile**, an operator-enabled remote-control mode for normal
   firmware use.

The harness must not become an unauthenticated field-control back door, and
Low Profile must not turn the Cardputer into a general-purpose shell.

## Goals

- Start/cancel Probe and observe its terminal state without pressing keys.
- Run a deterministic transmitter, Cardputer, and host loop for 1,000 cycles.
- Let an operator control safe, existing mission actions over a cable or BLE.
- Keep the SX1262 exclusively owned by `radio_task`; all remote actions use
  the same non-blocking request APIs as the on-device UI.
- Preserve the existing durable evidence model: `probe.csv` and `session.csv`
  decide pass/fail, not a terminal transcript.

## Boundaries and non-goals

- LoRaTrace remains RX-only. The test transmitter is a separately flashed
  Heltec V4, never the Cardputer.
- Low Profile exposes no shell, file-system access, arbitrary RF parameters,
  packet injection, config writes, WiFi/AP enablement, or arbitrary pin/SPI
  access.
- It does not make a second radio owner, poll the UI, or allocate an unbounded
  command/log buffer.
- "Bluetooth" means Bluetooth LE. ESP32-S3 has no Bluetooth Classic support.
- Phase 8 still ships its fixed candidate plan; remote candidate editing is
  not part of this work.

## Operator experience

The System menu gets a **Low Profile** item, off by default.

- Enabling it is an explicit on-device action and gives a visible active
  indication. It is session-only for the first release: a reset returns it
  to off.
- USB control is accepted only while that toggle is on. It is intended for an
  attached host, including the bench harness.
- BLE is separately enabled inside Low Profile. The first pairing opens a
  60-second, on-device-confirmed window and shows a fresh six-digit code.
  Thereafter a single bonded controller may reconnect; "Forget controller"
  is an on-device action.
- The name means discreet remote operation, not a new radio mode: Watch,
  Probe, Sweep, and the display/backlight keep their existing semantics.

The initial remote command allowlist is intentionally small:

| Command | Action |
| --- | --- |
| `HELLO`, `STATUS` | Version/build, capabilities, resolved state and counters |
| `TRACE_SET` | Existing pause/resume request |
| `PROFILE_SET` | Existing Meshtastic/MeshCore profile-switch request |
| `PROBE_START`, `PROBE_CANCEL` | Existing bounded Probe request/cancellation |
| `LOW_PROFILE_OFF` | Disable remote control locally |

`PROBE_START` retains its existing SD requirement. A request is an acceptance
acknowledgement only; `STATUS` reports asynchronous completion, cancellation,
failure, the active resolved frequency, candidate progress, and the cumulative
successful-home-restore counter so the harness can compare before/after
evidence.

## Control architecture

```text
USB serial ---------\
                     > Low Profile service -> fixed command mailbox -> existing APIs
BLE GATT RX --------/                                  |              |
                                                  UI actions       radio task
                                                                       |
                                                                   SX1262 only
```

The initial USB service is polled by `ui_task` on Core 0. It owns only
transport framing, bounded parsing, and response delivery. It never calls
RadioLib, SD, display, or WiFi APIs. Commands become existing request calls
(`radioRequestDiscoverySweep()`, profile switching, and Trace pause), so the
radio task still owns all retunes, CAD, receive windows, and home restoration.

### Wire protocol

Use an ASCII, line-framed protocol so diagnostic serial output remains useful:

```text
@LTRX/1 <sequence> <opcode> <arguments> <crc16>\n
@LTRX/1 17 PROBE_START - 8C31
@LTRX/1 17 ACK ACCEPTED 50A2
```

- A request/response line is at most 128 bytes; the parser has one fixed
  buffer and rejects oversized, malformed, wrong-version, or bad-CRC frames.
- Every command is sequence-numbered. The latest accepted sequence, command,
  argument, and response are retained so a host retry after USB/BLE loss
  cannot accidentally run an extra Probe; a new command that reuses a sequence
  after reconnect is not mistaken for that retry.
- Existing human diagnostic lines remain ordinary lines. The host only treats
  `@LTRX/1` records as protocol traffic. Responses use the Serial mutex and
  are emitted in one write.
- BLE transports the exact ASCII payload through one encrypted writable GATT
  characteristic and one encrypted notify characteristic. It does not invent
  a second command grammar.

### Security model

Low Profile broadens the documented serial-only attack surface, so it must
not be silently enabled.

- USB: command parsing is disabled unless the physical menu toggle is on;
  reset disables it. This is deliberate physical-presence authorization, not
  a claim that USB is secret.
- BLE: one controller, LE Secure Connections with MITM protection and the
  shown passkey, encrypted read/write/notify characteristics, and no command
  handling before bonding succeeds. Advertising is off until Low Profile BLE
  is enabled.
- All transports enforce the same allowlist, frame-size limit, rate limit,
  and status-only error messages. No received command is logged verbatim.
- A radio command does not outlive the mode: disabling Low Profile does not
  abandon an active Probe; the existing radio task completes cancellation and
  restores Watch first.

BLE is a memory and coexistence gate, not an assumption: establish USB first,
then measure BLE-on/BLE-connected heap, stack, and WiFi interaction against
the Phase 7 acceptance evidence before it is enabled in a release build.

## Deterministic Phase 8 harness

### Hardware

```text
host computer
  |- USB CDC -> Cardputer-Adv (LoRaTrace bench build)
  `- USB CDC -> Heltec V4 (LoRaTrace test-transmitter firmware)

Heltec RF -- fixed attenuator/coax fixture or shielded enclosure -- Cardputer RF
```

The fixture is closed and low-power. It is not an over-the-air field test
transmitter. Set attenuation only after confirming the parts' ratings and
applicable local regulations.

### Heltec test-transmitter firmware

The Heltec firmware is a small PlatformIO project, separate from this RX-only
repository firmware. The target is the operator-confirmed **Heltec WiFi LoRa
32 V4 R8** (ESP32-S3R8 + SX1262); its PA control is GPIO5, unlike prior V4
revisions. It has a bounded serial protocol to:

- select only a compiled-in, source-backed tuple;
- arm a tagged packet sequence at a host-relative deadline;
- report `ARMED`, `TX_STARTED`, `TX_DONE`, and radio errors with a sequence;
- force a quiet interval for false-positive controls; and
- cap power and packet rate in firmware.

It starts with the Phase 8 Meshtastic candidates. MeshCore/legacy tuples are
added only when their complete sourced parameters are established. A stock
Meshtastic image remains useful for interoperability confirmation, but this
firmware is the timing instrument for CAD measurements.

### Host harness responsibilities

The Python harness discovers both devices by serial VID/PID or explicit paths,
keeps DTR/RTS low, reconnects the Cardputer after reset, sends sequence-safe
commands, retains raw captures, and writes one machine-readable result per
cycle. It must:

1. prove the Cardputer reports the expected build and candidate-plan version;
2. establish a quiet baseline and measure CAD false positives;
3. transmit at controlled points in each candidate's CAD window and measure
   detection/miss rates;
4. run completion, cancellation, timeout, and injected-error paths;
5. complete 1,000 cycles without queue/row/drop or unexpected recovery
   regressions; and
6. retrieve/copy the run artifacts, verify `probe.csv`/`session.csv`, and
   write a location-redacted summary under `hardware-results/`.

The harness never treats a serial line as the definitive assertion: USB CDC
can clip under load. It correlates terminal events with the intact SD CSVs.

### Bench-only fault hooks

The Cardputer bench build adds a compile-time-only fault/control hook at the
existing acquisition boundaries: before candidate retune, after successful
retune, during CAD wait, during receive-on-hit, and before/after home restore.
The hook may request cancellation or force a named radio-operation failure;
it cannot issue arbitrary RadioLib calls. Every hook path must report its
terminal state and prove the resolved home configuration is back in Watch.

## Acceptance gates

1. Native tests cover frame parsing, CRC, size/replay/rate rejection, command
   allowlist, and result/status rendering.
2. USB bench build proves each action reaches the existing request API without
   a new SX1262 owner.
3. The hardware harness satisfies all Phase 8 automated-cycle, cancellation,
   Watch-restoration, and queue/memory criteria with retained artifacts.
4. Low Profile USB is manually tested disabled, enabled, malformed-input, and
   reset-disabled.
5. BLE is added only after its own pairing, unauthorized-client, reconnect,
   heap/stack, WiFi coexistence, and reset/forget-controller tests pass.

## Delivery order

1. Shared protocol core plus native tests.
2. USB Low Profile menu/task and bench build control endpoint.
3. Host harness and Heltec deterministic transmitter firmware.
4. Cardputer hardware validation and Phase 8 evidence reconciliation.
5. BLE transport and its separate measured acceptance gate.

This order makes the Phase 8 test instrument useful before any BLE memory or
security risk is accepted, while ensuring the production feature reuses a
tested command contract rather than inheriting bench shortcuts.
