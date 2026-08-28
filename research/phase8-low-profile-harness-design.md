# Phase 8 bench harness and Serial Control design

**Status:** active implementation plan, revised 2026-08-28.

This design has two deliberately separate consumers of one bounded command
core:

1. a trusted, wired Phase 8 lab harness that proves Probe's hardware exit
   criteria; and
2. **Serial Control**, an operator-enabled remote-control mode for normal
   firmware use.

The harness must not become an unauthenticated field-control back door, and
Serial Control must not turn the Cardputer into a general-purpose shell.

## 2026-08-28 revamp decision

The first 1,000-cycle run proved the nominal control path, but its original
host script had transport, framing, retry, capture, and scenario policy in one
file. That shape is now retired as an architecture: it would make every new
cancel/fault or bench-node test copy the same USB edge cases.

The replacement has one shared host core and thin scenario modules:

- `scripts/bench_harness.py` owns CRC framing, native-USB chunk recovery,
  explicit endpoint setup, sequence-safe retries, status parsing, and the
  append-only serial capture.
- `scripts/phase8_bench.py` is the nominal Probe scenario only; it owns the
  candidate timing and terminal assertions, not transport mechanics.
- `scripts/phase8_cancel_bench.py` is the first additional scenario; it owns
  only the RUNNING -> CANCELLED request and home-restore assertions.
- `scripts/phase8_fault_bench.py` arms one named, one-shot hook against the
  dedicated `cardputer-adv-bench` image and checks FAILED/CANCELLED plus home
  restoration.
- `scripts/phase8_contention_bench.py` runs the nominal Heltec pulse while
  polling Cardputer STATUS at a bounded interval, exercising the shared USB
  command path during acquisition.
- Future cancellation, fault-injection, contention, CAD-rate, and bench-node
  scenarios reuse the same core and write the same serial/console artifact
  pair. The launchers include `run_phase8_fault.sh`,
  `run_phase8_contention.sh`, and `run_phase8_cad_rate.sh`.

No legacy Phase 8 transport copy will be retained. The separate
`capture_serial_reconnect.py` utility remains a Phase 7 reconnect diagnostic,
not a Phase 8 scenario or alternate protocol implementation.

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
- Serial Control exposes no shell, file-system access, arbitrary RF parameters,
  packet injection, config writes, WiFi/AP enablement, or arbitrary pin/SPI
  access.
- It does not make a second radio owner, poll the UI, or allocate an unbounded
  command/log buffer.
- "Bluetooth" means Bluetooth LE. ESP32-S3 has no Bluetooth Classic support.
- Phase 8 still ships its fixed candidate plan; remote candidate editing is
  not part of this work.

## Operator experience

The System menu gets a **Serial Control** item, off by default.

- Enabling it is an explicit on-device action and gives a visible active
  indication. The physical enable is persisted in NVS, which is necessary
  because opening native USB serial resets this board; explicit on-device or
  remote disable turns it off.
- USB control is accepted only while that toggle is on. It is intended for an
  attached host, including the bench harness.
- BLE is separately enabled inside Serial Control. The first pairing opens a
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

`LOW_PROFILE_OFF` is retained as the wire opcode for compatibility; the
operator-facing feature name is **Serial Control**.

### Serial diagnostic policy

Serial Control and human diagnostics share the native USB endpoint, so the
firmware keeps them separate by policy rather than asking every host parser to
guess. Boot identity, fatal errors, and major lifecycle announcements remain
available. Periodic health/heap lines, backlight instrumentation, and per-
detection detail are Debug-gated and suppressed while Serial Control is on.
The Debug toggle therefore controls observability without contaminating the
machine-readable command channel; the actual health counters remain durable in
`session.csv`.

`PROBE_START` retains its existing SD requirement. A request is an acceptance
acknowledgement only; `STATUS` reports asynchronous completion, cancellation,
failure, the active resolved frequency, candidate progress, and the cumulative
successful-home-restore counter so the harness can compare before/after
evidence.

## Control architecture

```text
USB serial ---------\
                     > Serial Control service -> fixed command mailbox -> existing APIs
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

Serial Control broadens the documented serial-only attack surface, so it must
not be silently enabled.

- USB: command parsing is disabled unless the physical menu toggle is on.
  Its NVS-backed enable survives USB/software reset and power loss, but must
  be explicitly disabled. This is deliberate physical-presence
  authorization, not a claim that USB is secret.
- BLE: one controller, LE Secure Connections with MITM protection and the
  shown passkey, encrypted read/write/notify characteristics, and no command
  handling before bonding succeeds. Advertising is off until Serial Control BLE
  is enabled.
- All transports enforce the same allowlist, frame-size limit, rate limit,
  and status-only error messages. No received command is logged verbatim.
- A radio command does not outlive the mode: disabling Serial Control does not
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

The shared Python bench core discovers both devices by serial VID/PID or
explicit paths, keeps DTR/RTS low, reconnects the Cardputer after reset, sends
sequence-safe commands, and retains raw captures. Scenario modules add the
test-specific assertions and write one machine-readable result per cycle. The
nominal scenario and every future scenario must:

1. prove the Cardputer reports the expected build and candidate-plan version;
2. establish a quiet baseline and measure CAD false positives;
3. transmit at controlled points in each candidate's CAD window and measure
   detection/miss rates;
4. run completion, cancellation, timeout, and injected-error paths;
5. complete 1,000 cycles without queue/row/drop or unexpected recovery
   regressions; and
6. retrieve/copy the run artifacts, verify `probe.csv`/`session.csv`, and
   write a location-redacted summary under `hardware-results/`.

Every run has three distinct host artifacts: an append-only `.serial.log` for
the raw endpoint exchange, a `.console.log` for launch metadata and human
failure output, and a `.results.jsonl` file containing one structured object
per scenario event/cycle. A scenario may add more evidence, but it must not
merge human output into the protocol capture.

The harness never treats a serial line as the definitive assertion: USB CDC
can clip under load. It correlates terminal events with the intact SD CSVs.

### Bench-only fault hooks

The `cardputer-adv-bench` image adds a compile-time-only `BENCH_FAULT` command
at the existing acquisition boundaries: before candidate retune, after
successful retune, during CAD wait, during receive-on-hit, and before/after
home restore. Its argument is one bounded `POINT:CANCEL` or `POINT:FAIL`
operation, consumed once by the radio task. It cannot issue arbitrary RadioLib
calls, and the production `cardputer-adv` image returns `UNSUPPORTED`. Every
hook path reports a terminal state and still attempts resolved home
configuration restoration.

### Bench-only CAD selector and rate gate

The same image also adds `BENCH_CAD N`, where `N` is exactly 1, 2, 4, 8, or
16 SX1262 CAD symbols. It is intentionally absent from the normal Serial
Control allowlist and cannot cause an arbitrary radio operation: it only
chooses the next Probe's bounded CAD symbol configuration. The production
image returns `UNSUPPORTED`.

`phase8_cad_rate_bench.py` runs a distinct quiet-control/pulsed-fixture
window for every permitted value, preserving normal terminal, SD, and home
restoration assertions. A quiet target-bit hit in an exposed RF environment
is ambiguous (a genuine ambient preamble and a CAD false hit look the same),
so that run reports an observed room rate only. The false-positive/miss-rate
acceptance run needs a physically controlled quiet condition, such as a
shielded enclosure or an appropriately attenuated conducted fixture.

The contention scenario is intentionally scoped to serial/USB contention in
this slice: it floods idempotent STATUS requests while the Heltec pulse and
Probe acquisition run. WiFi-on/SPI-load contention remains a separate hardware
matrix item because Serial Control deliberately has no WiFi command.

The fault matrix runner keeps both endpoints open for one Cardputer boot. A
first multi-process attempt was invalidated by repeated native-USB reset churn:
the Cardputer reported SD CRC/no-token failures, `SD=0`, and a logger watchdog
reset before the protocol became available. The stable-boot runner is the
accepted method; a clean `SD=1` boot is a prerequisite before interpreting any
remaining matrix result.

## Acceptance gates

1. Native tests cover frame parsing, CRC, size/replay/rate rejection, command
   allowlist, and result/status rendering.
2. USB bench build proves each action reaches the existing request API without
   a new SX1262 owner.
3. The hardware harness satisfies all Phase 8 automated-cycle, cancellation,
   Watch-restoration, and queue/memory criteria with retained artifacts.
4. Serial Control USB is manually tested disabled, enabled, malformed-input,
   USB-reset reconnect, and explicit-disabled.
5. BLE is added only after its own pairing, unauthorized-client, reconnect,
   heap/stack, WiFi coexistence, and reset/forget-controller tests pass.

## Delivery order

1. Shared protocol core plus native tests.
2. USB Serial Control menu/task and bench build control endpoint.
3. Shared host harness core, scenario modules, and Heltec deterministic
   transmitter firmware.
4. Cardputer hardware validation and Phase 8 evidence reconciliation.
5. BLE transport and its separate measured acceptance gate.

This order makes the Phase 8 test instrument useful before any BLE memory or
security risk is accepted, while ensuring the production feature reuses a
tested command contract rather than inheriting bench shortcuts.
