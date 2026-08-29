# LoRaTrace RX — Changelog

Rolling, chronological decisions/change log — every dated entry
explains what changed, why, and how it was verified (bench/hardware
vs. host-native tests only). Moved out of PROGRESS.md (2026-08-25)
once that file's own decisions-log tail had grown past the point of
being a status doc; PROGRESS.md now holds current status, the
build-order checklist, open questions, and next steps, and points
here for full history. See ROADMAP.md for phase-by-phase scope and
src/version.h for the versioning convention (MAJOR.MINOR = phase,
PATCH = fix with no new phase scope).

- **2026-08-29 — Probe/Sweep card UI polish, two operator feedback
  rounds on real hardware.** Sweep gained a peak-bin occupancy bitmask
  (tick marks along the frequency bar, sized to match the position
  marker) and a strongest-peak frequency/RSSI callout. Probe gained a
  decoded candidate-name readout (`uiDiscoveryCandidateLabel()`,
  `ui_labels.h`) and a plain-English "N/M channels active" headline, with
  the raw hits/free/timeout/err counts kept as a reference line after
  feedback that the redesign felt too sparse. Both cards' headline now
  fades to a dim "IDLE" 8 seconds after a terminal result. Fixed a
  digit-shortcut regression the six-page carousel introduced: JUMP_1..5
  were still pointing at Sweep's pre-insertion targets, so pressing "3"
  landed on CHANNEL while the footer read "4/6" — retargeted to match each
  page's real position and added `JUMP_6`→SYSTEM (`K=31` for `'6'`,
  already corroborated by `test_keyboard`'s own prior "unrelated keys"
  check). Full detail in PROGRESS.md's Phase 9 checklist.

- **2026-08-29 — Sweep noise-floor margin recalibrated to 35.0dB; a
  production/bench default-propagation bug caught by hardware
  verification.** A 3-repeat confirmatory matrix at 20-40dB (same bench as
  below) found 35.0dB the first margin with a genuine 0/3 quiet
  false-trigger rate across 221 bins, while still registering injected
  LongModerate pulses more reliably than the only other zero-false-
  positive point tested (40.0dB: 1.9% vs 0.8% active hit rate).
  `ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10` (`energy_observation.h`) moved
  from the 100 (10.0dB) placeholder to 350 (35.0dB). The first real sweep
  on **production** firmware after that change still measured 96/221
  (43%) peaks, unchanged — `bench_fault.cpp`'s `benchSweepMarginDbmX10()`
  (the function `radio_task.cpp` actually calls) had its own hardcoded
  `100` literal in the production-branch return and the bench image's
  pre-override initial value, copied from the constant instead of
  referencing it, so the constant edit alone did nothing at runtime.
  Fixed by having both read `ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10`
  directly. Re-verified: three consecutive production-firmware sweeps
  measured 0/221 peaks, `COMPLETE`, and a correctly restored home
  frequency every time. Recorded as a general lesson, not just a Sweep
  one: a constant change plus a green test suite is not evidence a
  runtime default actually moved — checking a live device against the
  calibration bench's own expected number is what caught this, and
  nothing in host or bench-image test coverage would have. Full matrix in
  PROGRESS.md's Phase 9 checklist.

- **2026-08-28/29 — Phase 9 Sweep margin calibration bench built and run.**
  New bench-image-only `BENCH_SWEEP_MARGIN` opcode
  (`src/bench_fault.h`/`.cpp`) mirrors `BENCH_CAD`'s exact production/bench
  split — production firmware always returns the placeholder 10.0dB
  default and rejects changes. `radio_task.cpp`'s `performEnergySweep()`
  now reads the margin through `benchSweepMarginDbmX10()` instead of a
  hardcoded default, so `scripts/phase9_sweep_margin_bench.py` can sweep
  margin values across repeated Sweeps without a reflash per value, using
  the Heltec as a known LongModerate injection source for a quiet-vs-active
  comparison at each margin (same shared `bench_harness.py` transport
  Phase 8's own scripts use). Same standing limitation already on record
  for Phase 8's CAD calibration: no known-quiet RF control exists on this
  bench, so results are a real room-rate characterization, not a
  calibrated false-positive/miss rate. First matrix (1 trial/margin,
  5.0-30.0dB) found the shipped 10.0dB default sitting at 45-51% peak
  rate — confirming Phase 9 slice 2's hardware finding with a controlled,
  scripted comparison — while 20-25dB delta values (+11 to +20 peaks
  quiet-vs-active, out of 221 bins) show the mechanism responds correctly
  to real injected RF rather than pure noise, and 30.0dB is the first
  point resembling DESIGN.md's "sparse, only real peaks" intent. Full
  numbers in PROGRESS.md's Phase 9 checklist. Host tests (133/133,
  including new `BENCH_SWEEP_MARGIN` round-trip coverage) and both the
  production and `cardputer-adv-bench` builds pass.

- **2026-08-28 — Serial Control firmware-side rename completed.** The
  2026-08-28 diagnostic-policy work renamed the operator-facing feature to
  Serial Control but deliberately kept internal `lowProfile*` C++ symbols,
  file names, and the `LowProfileOpcode`/`LowProfileFrame` types as
  compatibility names pending a later cleanup. That cleanup is now done:
  `src/low_profile.h`/`.cpp`/`low_profile_protocol.h` are
  `src/serial_control.h`/`.cpp`/`serial_control_protocol.h`, every
  `lowProfile*` function/type is `serialControl*`/`SerialControl*`, and
  `MenuAction::LOW_PROFILE_TOGGLE` is `SERIAL_CONTROL_TOGGLE`. The actual
  compatibility surface — the NVS namespace/key strings (`"lowprofile"`/
  `"usb_enabled"`) and every wire opcode name, including the still-literally-
  named `LOW_PROFILE_OFF` opcode — was deliberately left untouched, since
  those (not the C++ symbol names) are what a reconnecting host or an
  already-provisioned device's persisted enable-state actually depend on.
  Verified compatible on real hardware: the exact same `HELLO`/`STATUS`
  request produced byte-for-byte identical wire output before and after the
  rename, and `SWEEP_START`/`SWEEP_CANCEL` (added the same session) round-
  tripped correctly post-rename too. Host tests (132/132, including the
  renamed `test_serial_control_protocol`) and both the production and
  `cardputer-adv-bench` builds are unaffected — identical binary sizes to
  the pre-rename build, as expected for a pure rename.

- **2026-08-28 — Phase 8 closed as v0.8.0.** The replacement-card durable
  CSV review found stable headers, zero malformed/non-terminated rows, and
  intact Probe/session/detection records across the retained runs. The
  100-cycle USB-contention repetition completed every Probe and serviced 668
  framed STATUS requests during 478.7 seconds with no terminal or home-restore
  failure. For application interoperability, an externally flashed official
  Meshtastic Heltec V4 R8 was configured to LONG_MODERATE and sent ten live
  packets through one persistent API connection while the Cardputer Probe ran;
  the terminal was `B=COMPLETE`, `SD=1`, `R=1`, `C=6,2,0,0`, with the expected
  version-2 LongModerate candidate bit in `M=0024`. The repository's
  deterministic Heltec bench image was restored afterward. CAD `symNum` remains
  at the upstream two-symbol setting: the available bench cannot create a
  known-quiet RF control, so no statistical false-positive/miss rate or
  production retune is claimed. Transient mode and calibrated CAD work are
  explicitly post-Phase-8 follow-up. Production firmware is v0.8.0.
  Evidence: `hardware-results/private/phase8/phase8-meshtastic-longmod-interop-20260828T2052.serial.log`.

- **2026-08-28 — CAD antenna-path isolation and retained quiet-only evidence.**
  The lower-gain four-symbol fixture link passed 20/20 capped Heltec pulses
  without a CAD timeout, but its quiet target bit remained active in 14/20
  controls. Local-mesh-off and metal-ammo-box antenna-attached preflights
  still produced two target hits in three quiet controls while retaining 3/3
  fixture pulses. A new observe-only quiet-only harness mode then tested the
  Cardputer with its receive antenna disconnected: zero LongModerate target
  hits in ten quiet Probes, although unrelated candidate activity remained.
  This isolates the recurring target activity to the receive antenna path;
  it does not establish a calibrated outside-RF source or a universal CAD
  false-positive rate. Production remains on the source-backed two-symbol
  setting. Artifacts: `phase8-cad-rate-20260828T200710Z.*`,
  `phase8-cad-rate-20260828T201901Z.*`,
  `phase8-cad-rate-20260828T202210Z.*`, and
  `phase8-cad-rate-20260828T202539Z.*` under
  `hardware-results/private/phase8/`.

- **2026-08-28 — Phase 8 CAD-rate harness and room-rate pilot.** Added a
  compile-time bench-only `BENCH_CAD` selector limited to SX1262's supported
  1/2/4/8/16-symbol CAD windows; normal firmware rejects it, and the radio
  task remains the sole SX1262 owner. The shared rate harness checks every
  Probe's terminal state, SD health, home restoration, fixture `TX_DONE`,
  target candidate bit, and CAD timeout count. Its first exposed-room
  observation pilot ran three quiet and three capped -9 dBm LongModerate
  pulses per window: all 30 Probes completed with `SD=1`, restored home, and
  detected every fixture pulse. Quiet target-bit activity was respectively
  0/3, 2/3, 2/3, 1/3, and 0/3 for 1/2/4/8/16 symbols, proving the room cannot
  supply a known-quiet false-positive control. Sixteen symbols also timed out
  once on each cycle, so zero CAD timeouts is now an explicit strict-gate
  requirement. This is useful correlation and a complete harness check, not
  a CAD tuning verdict; the 20/20 rate gate awaits an attenuated or shielded
  quiet fixture. Artifact: `phase8-cad-rate-20260828T190616Z.*` under
  `hardware-results/private/phase8/`.

- **2026-08-28 — MeshOregon candidate-specific Probe pilot.** Replaced the
  obsolete OregonMesh fixture tuple with the operator-supplied current
  MeshOregon physical configuration: 918.5 MHz, SF8, BW125, CR4/5. The
  community-specific candidate is first in discovery-plan version 2, avoiding
  later-candidate timing distortion from ambient CAD receive windows; names
  and PSKs are intentionally absent because CAD needs neither. The capped
  -9 dBm Heltec fixture completed four-for-four target-bit pulses (`M & 0001`)
  with `TX_DONE`, `SD=1`, zero timeout/error, and Watch restored to 906875 on
  every run. A three-cycle no-transmit control had no MeshOregon candidate-bit
  hits, but two runs saw unrelated candidate activity, so it is explicitly
  preliminary correlation—not a false-positive/miss-rate result. Artifacts:
  `phase8-meshoregon-20260828T170023Z.*`,
  `phase8-meshoregon-quiet3-20260828T170300Z.*`, and
  `phase8-meshoregon-pulse3-20260828T170400Z.*` under
  `hardware-results/private/phase8/`.

- **2026-08-28 — Curated source boundary and candidate-specific CAD evidence.**
  Rechecked the fixed Phase 8 tuple set against current Meshtastic and
  MeshCore primary documentation. Meshtastic custom channel-name hashes are
  not enumerable as a universal fixed plan, so Probe remains bounded to its
  standard anchors plus a known per-profile override; MeshCore's current
  USA/Canada narrow tuple remains source-backed at 910.525 MHz/SF7/BW62.5/
  CR5. Serial Control STATUS now emits compact per-Probe CAD counts and a
  candidate-index mask, allowing the shared `phase8_cad_bench.py` harness to
  distinguish a fixture's intended candidate from unrelated live RF. After a
  rejected arm-after-Probe calibration (`M=80`), pre-arming the capped -9 dBm
  LongModerate Heltec produced the target bit (`M & 0x02`) on two runs with
  matching TX_DONE, SD intact, zero timeout/error, and home restoration; a
  quiet control produced `M=00`. The second target run also had ambient
  candidate hits, so this is targeted tuple evidence—not a claim of a
  noise-free environment or a statistical false/miss rate. Artifacts:
  `phase8-cad-pulse-target-20260828T164413Z.*`,
  `phase8-cad-pulse-target-20260828T164509Z.*`, and
  `phase8-cad-quiet-mask-20260828T164443Z.*` under
  `hardware-results/private/phase8/`. Native tests remain 104/104 and the
  production Cardputer build/flash passed.

- **2026-08-28 — v0.7.1 SD recovery hardening.** The logger now adopts the
  successful boot-time SD mount instead of forcibly calling `SD.end()` and
  remounting it during task startup. Automatic failed-card remount attempts
  were removed: a missing or electrically sick card leaves RX/Watch alive
  with logging offline, rather than repeatedly entering synchronous SD I/O.
  System > **Retry SD** queues one explicit remount after a physical reseat.
  On-device verification covered no-card boot (device remains alive) and a
  reseat followed by Retry SD (logging returns with `SD=1`). Native tests
  pass 104/104; production and bench builds succeed.

- **2026-08-28 — Complete stable-boot Phase 8 fault matrix passed.** With
  the replacement card reporting `SD=1`, the Cardputer/Heltec V4 R8 matrix
  passed every CANCEL and FAIL action at BEFORE_RETUNE, AFTER_RETUNE,
  CAD_WAIT, RX_WAIT, HOME_RESTORE_BEFORE, and HOME_RESTORE_AFTER. Every
  terminal record restored `F=906875` and recovery advanced `R=1..12`.
  The RX_WAIT fixture now arms the Heltec before Probe begins so its
  LongModerate packet occupies the first non-home window; the matrix also
  uses a one-second inter-case USB settle interval. Evidence is retained in
  `hardware-results/private/phase8/phase8-fault-matrix-20260828T160815Z.*`.

- **2026-08-28 — Phase 8 USB-contention evidence passed.** The bench image
  completed 100/100 Probe cycles while the host issued STATUS requests at a
  25ms target cadence. Every terminal record was `COMPLETE`, retained
  `SD=1`, restored `F=906875`, and advanced recovery `R=1..100`; 668 framed
  STATUS responses were validated. Native USB text still occasionally clips
  or duplicates a frame, so the CRC-framed parser and JSONL terminal records
  remain authoritative. Production v0.7.1 was then restored and confirmed
  without bench capability, with Watch active and `SD=1`. Evidence is under
  `hardware-results/private/phase8/phase8-contention-100-20260828T160944Z.*`.

- **2026-08-27 — Phase 8 research/foundation slice started.** Added
  `research/phase8-discovery-research.md` and the pure, fixed
  `src/discovery_plan.h` candidate layer with host coverage. Meshtastic
  candidates use the upstream bandwidth-specific slot formula; ShortTurbo is
  excluded because its US centre is outside the Cap's supported band. Only
  source-backed MeshCore tuples are included; legacy coding-rate guesses stay
  out. No version bump yet: bounded radio acquisition and hardware gates are
  not complete.

- **2026-08-28 — Nominal Phase 8 automated bench gate completed.** The
  Cardputer/Heltec V4 R8 harness completed 1,000 Probe runs in 2h17m31s:
  every cycle reached `B=COMPLETE`, restored the resolved home channel
  (`918500` kHz), advanced recovery `R=1..1000`, and produced a matching
  `TX_DONE OK` event. The final Heltec `QUIET` acknowledgement arrived and no
  harness failure occurred. One `TX_STARTED` line (sequence 322) was clipped
  in the native USB capture while its `TX_DONE OK` remained intact, so the
  serial transcript is supplemental to the terminal/home-restore assertions.
  Raw and console artifacts are retained under
  `hardware-results/private/phase8/phase8-1000-20260828T053715Z.*`.

- **2026-08-28 — Harness and serial-observability revamp accepted.** The
  one-file Phase 8 host script is no longer the architecture: shared framing,
  CRC recovery, endpoint setup, retries, status parsing, and capture now live
  in `scripts/bench_harness.py`; `scripts/phase8_bench.py` contains only the
  nominal scenario. Future cancellation, fault, contention, and bench-node
  scenarios must reuse that core; the first cancellation scenario is now
  `scripts/phase8_cancel_bench.py`. All scenarios keep separate `.serial.log`,
  `.console.log`, and `.results.jsonl` artifacts. The user-facing firmware feature is renamed from
  Low Profile to **Serial Control**; internal NVS and wire identifiers remain
  compatible until the firmware-side rename is implemented. The tracked plan
  and acceptance gates are in `research/phase8-low-profile-harness-design.md`.

- **2026-08-28 — Serial diagnostic policy foundation implemented.** The
  System menu and toasts now call the feature **Serial Control** while the
  internal `lowProfile*` functions, NVS namespace, and wire opcode remain
  stable for host compatibility. Periodic `[status]`, `[mem]`, and backlight
  instrumentation now requires Debug and is suppressed while Serial Control
  is enabled; boot/fatal/lifecycle messages and framed command responses stay
  available. Cardputer build succeeds with 50,900 bytes static RAM and
  983,925 bytes flash; native tests remain 104/104.

- **2026-08-28 — Bench fault and contention scenarios added.** Added the
  compile-time-gated `cardputer-adv-bench` image and bounded `BENCH_FAULT`
  command. Named one-shot hooks cover candidate retune, CAD wait, receive
  wait, and both sides of home restoration; production firmware returns
  `UNSUPPORTED`. Added shared-core `phase8_fault_bench.py` and
  `phase8_contention_bench.py` scenarios plus durable launchers. A real
  `CAD_WAIT:FAIL` smoke reached `B=FAILED`, restored `F=918500`, and advanced
  recovery `R=1`; a three-cycle USB-contention smoke also completed with
  intact `TX_DONE` events and home restoration. The remaining fault matrix
  and long contention run are still open evidence.

- **2026-08-28 — Fault-matrix hardware attempt paused by SD failure.** The
  initial per-case runner was replaced with a stable-boot matrix after reset
  churn caused invalid transport results. The stable-boot retry captured
  `sdCommand()` CRC/no-token errors, `SD=0`, and a logger task watchdog reset;
  Probe correctly refused to start without its required datastore. These
  artifacts are retained as a boot/SD reliability finding, not counted as
  fault-hook failures. Reseat or replace the microSD card and confirm `SD=1`
  before rerunning the matrix.

- **2026-08-27 — Phase 8 bounded Probe acquisition slice added.** Added a
  radio-task-owned, deadline-bounded CAD/receive-on-hit sweep using the fixed
  candidate plans, explicit two-symbol CAD, home-configuration restoration,
  cancellation/recovery counters, and a separate fixed `ScanObservation`
  queue. Probe observations are stamped by the logger into durable
  `probe.csv`, cumulative totals append to `session.csv`, and the global P-key
  shortcut plus Enter on the Probe card expose start/cancel; Enter on the
  Radio card toggles Trace. An explicit `WATCH PAUSED` display state and a
  retained second-card results page keep a short successful scan visible.
  Native tests pass 100/100 and the final
  Cardputer-Adv build succeeds at 50,492 bytes RAM and 978,489 bytes flash;
  no version bump or hardware claim is made until the RF/recovery gates run.


- **2026-08-12** — Board id: used `esp32-s3-devkitc-1` in `platformio.ini`.
  No dedicated PlatformIO board definition for Cardputer-Adv was found;
  this is what's used in practice by the M5Stack/ESP32-S3 community for
  this chip. Revisit if M5Stack ships an official board file.
- **2026-08-12** — Deferred creating `radio_task`/`gps_task`/
  `logger_task`/`ui_task`/`fingerprint.h` (CLAUDE.md's proposed layout)
  until Phase 2. CLAUDE.md's Status section explicitly says start with
  phase 1 bring-up "before touching the task/queue architecture" — the
  scaffold respects that rather than creating empty stub files now.
- **2026-08-12** — Added `src/board_pins.h`, which isn't in CLAUDE.md's
  proposed layout. Justification: DESIGN.md §1 already tabulates the full
  pin map as decided config, multiple future tasks (radio, GPS, logger)
  will need the same constants, and Phase 1 needs them immediately — a
  shared header avoids duplicating pin numbers across files as they're
  added in later phases.
- **2026-08-12** — RadioLib pinned to `^7.7.1` (latest release as of this
  session). PI4IOE5V6408 I2C address (0x43) and register offsets
  (0x03 direction / 0x05 output / 0x07 high-Z) sourced from ESPHome's
  component docs and a from-datasheet MicroPython driver, not the primary
  Diodes datasheet — flagged for hardware verification, not treated as
  ground truth.
- **2026-08-12** — Confirmed (user): returning from a Launcher-installed
  app to Launcher itself is a manual restart + button combo, not a
  software hook our firmware needs to implement. No return-to-launcher
  code needed in `main.cpp`/future `ui_task`.
- **2026-08-12** — Settings/config get persisted to a folder on the SD
  card, not flash NVS. Rationale: Launcher owns the flash partition table
  dynamically per installed app (see ROADMAP.md Distribution section), so
  a custom NVS/data partition isn't guaranteed to survive the user
  swapping firmwares. This is also just the existing DESIGN.md philosophy
  ("SD is the datastore, RAM is a relay buffer") extended to config, not a
  new pattern. No config schema/format decided yet — deferred until
  something actually needs to be configurable (Phase 2+ profile selection
  persistence being the likely first case).
- **2026-08-12** — Added `src/version.h` as the single source of truth for
  `FIRMWARE_VERSION`, printed on the boot banner. Added CI
  (`.github/workflows/build.yml`: `pio run -e cardputer-adv` +
  `pio test -e native` on every push/PR) and a tag-triggered release
  workflow (`release.yml`: `vX.Y.Z` tag -> builds, renames to
  `LoRaTraceRX-<version>.bin`, opens a **draft** GitHub Release — draft on
  purpose, since hardware verification is still pending and nothing should
  auto-publish yet). Added `[env:native]` to `platformio.ini` for
  host-based unit tests (no ESP32 toolchain needed) and a first real test
  (`test/test_channel_plans/`) validating the RF constants are in-band and
  don't collide — both verified to actually pass in this session, not just
  written. Note: `pio run` alone builds *all* environments including
  `native`, which has nothing to build outside `pio test` — always use
  `pio run -e cardputer-adv` (README/CI already do).
- **2026-08-12** — Added a rolling `dev-latest` release to `build.yml`:
  every push to `main` force-moves a `dev-latest` git tag and
  updates/republishes a prerelease at that tag with a fixed-filename
  binary (`LoRaTraceRX-dev.bin`), so there's always a stable, no-login
  download URL for SD-drop/Launcher testing without waiting on a version
  tag. Separate from `release.yml`'s versioned `vX.Y.Z` releases, which
  stay manual/deliberate. **Unverified**: this is the first time the
  workflow exists, so it hasn't actually fired on a real push yet — the
  `softprops/action-gh-release` update-in-place behavior for a re-used tag
  is a well-established pattern for this action, but confirm the first
  run actually updates `dev-latest` rather than erroring.
- **2026-08-12** — Added SD-based channel config
  (`src/config.h`/`config.cpp`, path `/loratrace/config.txt`,
  `sd-template/loratrace/config.txt` as the copyable example) so operators
  running non-default regional presets (e.g. MeshOregon) can override
  freq/SF/BW/CR without a rebuild. Scoped narrowly: one-shot boot-time SD
  read before `radio.begin()`, not the general Phase 2 settings/Logger
  architecture — this was pulled forward because it was blocking real
  testing tonight, not because Phase 1's scope changed otherwise. Values
  are bounds-checked (868-928MHz, SF5-12, CR5-8) and rejected
  field-by-field with a warning rather than applied blind, since a bad
  frequency/SF here means silently hearing nothing. Confirmed
  build-clean; SD mount itself is unverified pending the `PIN_SD_CS`
  hardware check above. This closes out the "no config schema decided
  yet" note in the previous entry.
- **2026-08-12** — Re-verified the SD/radio shared-bus finding against
  M5Stack's official docs directly (Cardputer-Adv microSD table + Cap
  LoRa-1262 SPI table, both re-fetched with a stricter verbatim-quote
  prompt after a first, hastier attempt produced a scrambled and
  internally-contradictory pin table — that error is worth remembering,
  not just fixing). User-supplied photo of the Cap LoRa-1262's printed pin
  diagram independently confirmed the SX1262 pins, the antenna-switch
  I2C address (0x43, now primary-source-confirmed, silkscreened on the
  module), and GPS UART pins. One correction found: DESIGN.md called the
  GPS chip "AT6668," official docs/diagram say "ATGM336H" — fixed, naming
  only, no pin/code impact. `board_pins.h` and DESIGN.md §1/§7 updated
  with the stronger sourcing.
- **2026-08-22** — First real-hardware boot (via Launcher SD-drop, not
  direct USB flash). Two back-to-back serial captures, read line by line:
  - Both show a clean run through to `Listening...`: antenna-switch
    IO-expander init succeeded (no FATAL hang), `SX1262 initialized.` with
    the default Meshtastic LongFast (US) channel active
    (906.875MHz/SF11/BW250/CR4:8), and no crash/reboot loop. This is the
    first empirical confirmation of I2C@0x43 register writes, FSPI
    availability, and RadioLib 7.7.x's runtime behavior on this exact
    board — see checklist above for which specific items this closes out.
  - Both also show `[config] /loratrace/config.txt not found — using
    built-in default channel.` — this is `config.cpp`'s *file-not-found*
    message, only reachable after `SD.begin(PIN_SD_CS, radioSPI)` already
    returned true (the "no SD card detected (or mount failed)" message is
    a distinct branch). That means the microSD card **mounted
    successfully** on the shared SPI bus — real hardware confirmation of
    the DESIGN.md §7 finding, not just docs. The override itself is still
    unverified since no `config.txt` was on the card.
  - The second capture starts from a true power-on and includes boot noise
    worth recording so it isn't mistaken for a firmware bug later:
    `rst:0x15 (USB_UART_CHIP_RESET)` is the normal reset every native-USB-
    CDC ESP32-S3 does when a serial terminal opens the port (DTR toggle) —
    expected given `ARDUINO_USB_CDC_ON_BOOT=1`, not a crash signature.
    `E (103) esp_core_dump_flash: Incorrect size of core dump image:
    18023945` is a stock ESP-IDF boot-time sanity check
    (`components/espcoredump`) finding non-erased, non-core-dump-shaped
    data at whatever flash offset the currently-active partition table
    calls "coredump" — non-fatal, and boot continues immediately after it.
    Likely explanation given the sideload path: Launcher owns the actual
    running partition table on a sideloaded install (ROADMAP.md
    Distribution section), which doesn't necessarily line up with this
    repo's own `default_8MB.csv`, so this offset can hold leftover bytes
    from whatever else has occupied flash there. Not produced by any code
    in this repo (confirmed via grep — no match for the message text or
    `esp_core_dump`/`esp_reset_reason` anywhere in `src/`). Likewise,
    `[boot] Turned on because (1= POWERON_RESET or 5==ESP_RST_DEEPSLEEP)
    --> 21` does not appear anywhere in this repo either — it's printed by
    Launcher itself before chain-loading `LoRaTraceRX-dev.bin`, not by our
    code (also worth noting: `21` isn't a value `esp_reset_reason_t`
    defines, reinforcing that this is Launcher's own, unrelated enum, not
    an ESP-IDF reset reason our code would ever need to interpret).
  - Net: no evidence of a firmware crash or panic anywhere in this log.
    Real gaps still open: no `[RX]` line (no live packet decoded this
    session) and the SD-config override path untested. See Next steps.
- **2026-08-22** — Investigated the "can't get back to Launcher without
  reflashing" report by cloning `bmorcelli/Launcher` (read-only) and
  reading its actual boot logic rather than guessing. Full finding
  recorded under "Open questions — Launcher distribution" above. While in
  there, also found (informational only, no action taken): Cardputer ADV
  uses a TCA8418 I2C keyboard controller (addr 0x34) on the *same* SDA/SCL
  pins (G8/G9) this repo already uses for the PI4IOE5V6408 antenna switch
  (0x43) — normal shared-I2C-bus usage, not a conflict, and our own boot
  log already shows the antenna-switch writes succeeding on real hardware.
  Their Cardputer-ADV notes separately mention GPIO5 (== `PIN_LORA_NSS`
  here) needing to be driven high during early GPIO init on that revision
  to avoid SD-mount interference from that same I2C device cluster — noted
  in `board_pins.h` for future reference, not acted on, since our own SD
  mount and radio init both already succeeded without it in the captured
  boot log.
- **2026-08-22** — `/loratrace/config.txt` is now auto-created (pre-filled
  with the current defaults) the first time this firmware sees a card that
  doesn't have one, instead of requiring an operator to hand-copy
  `sd-template/loratrace/` over themselves (`src/config.cpp`
  `writeDefaultConfig()`, gated behind `SD.exists()` so it never touches a
  file that's already there). Also fixes the noisy
  `[E][vfs_api.cpp] open(): ... does not exist, no permits for creation`
  line seen in the first hardware boot log's second capture — that came
  from opening a nonexistent file in read mode; after the first boot with
  a card, the file exists, so that path isn't hit again.
  `sd-template/loratrace/` stays around as an offline/reference copy.
  Build-clean; not yet bench-tested (needs a blank card through a real
  boot — see Phase 1 checklist).
- **2026-08-22** — Added a boot-status splash on the ST7789 LCD
  (`main.cpp` `splashLine()`/`initDisplay()`), mirroring the serial
  banner's milestones one line at a time as `setup()` reaches them,
  including the FATAL branches — so a hard failure shows on-screen too,
  not just over serial. **Explicit, narrow exception to CLAUDE.md's Phase
  1 Status text ("no UI yet")**, made at the user's direct request after
  the first hardware boot "looked like a brick" without a serial
  connection open, confirmed with the user before writing any code. Scoped
  the same way the SD-config read was: one-shot, setup()-only, no keyboard
  reading, no menus, no redraw loop — `ui_task.cpp` and real interactivity
  still wait for Phase 6. Pins, panel size, and the IPS column/row offset
  pairs (`board_pins.h`) are sourced from `bmorcelli/Launcher`'s own
  confirmed-working Cardputer/Cardputer-ADV config
  (`boards/m5stack-cardputer/platformio.ini` in that repo), not guessed —
  same sourcing discipline as the rest of this project's hardware facts,
  though still flagged TODO(verify) until bench-tested here specifically.
  Library: `moononournation/GFX Library for Arduino`, **pinned to exactly
  `1.4.0`, not caret-ranged** — actually ran `pio run -e cardputer-adv`
  (not just reasoned through) and found releases from ~1.5 onward require
  `esp32-hal-periman.h`, which only exists on Arduino-ESP32 core 3.x; this
  project's unpinned `platform = espressif32` currently resolves to core
  2.0.17, and 1.4.0 confirmed building clean against it. Also used the
  same real build to confirm both `pio run -e cardputer-adv` and `pio test
  -e native` still pass after all of tonight's changes, not just the
  display piece. Own SPI host (HSPI), pins fully disjoint from
  radioSPI/SD's FSPI pins — keeps DESIGN.md §1's bus-isolation rule intact
  rather than adding a third device to the already-shared radio/SD bus.
- **2026-08-22** — Follow-up from the boot-status splash: the splash is
  fully static once `setup()` returns (`loop()` never touches `tft`), so
  it stays on screen indefinitely showing whatever the last drawn line
  was — it does not flash briefly and disappear. That's also its weakness:
  a genuine hang (stuck in a FATAL `while(true)` loop, or wedged before
  ever reaching `loop()`) looks pixel-identical to a healthy idle screen.
  Added `heartbeatTick()` (`main.cpp`): a small dot in the bottom-right
  corner toggled every ~500ms, called from the top of `loop()` — freezes
  right alongside everything else if `loop()` stops running, which is the
  point, not a bug to fix. Still passive (no keyboard reading), so it
  doesn't cross the same CLAUDE.md Phase 6 boundary the splash itself was
  already granted an exception for.
- **2026-08-22** — Investigated "flipping the physical power switch does
  nothing, firmware just keeps running." Confirmed against M5Stack's own
  Cardputer-ADV docs (charging requires the switch ON; standby current
  with the switch OFF is ~0.23uA, i.e. genuinely off) plus a full read of
  every board-init path in `bmorcelli/Launcher`'s Cardputer/ADV code — no
  GPIO anywhere reads this switch. Conclusion, MEDIUM confidence (docs +
  absence-of-evidence in Launcher's source, not a schematic): the switch
  sits in the *battery* path only: OFF stops charging and stops the
  battery powering anything, but USB power (bench testing, exactly how
  this has been tested so far) feeds the regulator directly regardless of
  switch position, so the MCU never sees the switch move at all — not a
  firmware bug, nothing to "respect" in code, because there's no signal
  reaching the firmware to respect. Only affects standalone battery
  operation, unplugged from USB. If what's actually wanted is a
  keyboard-triggered manual sleep/power-down *while on USB* for bench
  testing, that's a different, real feature — but it needs keyboard
  reading, which is a new crossing of the Phase 6 UI boundary beyond what
  the boot splash was already granted, so it needs its own go/no-go before
  writing it, same as the splash did.
- **2026-08-22** — Investigated "Launcher goes by so fast we can't stop
  it." Same root cause as the Launcher return-to-menu finding above (the
  ~5s `bootToApp` window) — Launcher polls for a keypress every ~10ms
  (`vTaskDelay(pdMS_TO_TICKS(10))` in its input loop), so *holding a key
  down through the reset* (rather than watching the screen and reacting
  after the fact) reliably catches that window regardless of exact
  timing. Passed on to the user as the practical fix; the durable fix is
  still the "Boot to Launcher" Settings toggle documented above, once
  they've caught the window one time to reach Settings and flip it.
- **2026-08-22** — Two small additions ahead of the next hardware test
  round, packed into the same PR rather than costing a separate
  flash/test cycle: a boot-time `ESP.getFreeHeap()` snapshot (serial +
  splash), a first real data point for the open heap question above; and
  a splash line reporting whether the active channel is the hardcoded
  default or an SD override (`main.cpp`, next to the existing `[config]`
  serial messages), so the SD-config test below doesn't need a serial
  connection open to confirm it worked.
- **2026-08-22** — Direct USB flash (`pio run --target upload`, Windows/
  VSCode), first time this repo's own board def/partition table has run on
  real hardware instead of a Launcher sideload. `esptool` output: chip
  correctly identified as ESP32-S3 (QFN56) rev v0.2, Embedded Flash 8MB
  (GD), and bootloader + partition table + app all wrote and verified
  cleanly at the standard offsets (0x0/0x8000/0x10000). Note: those same
  offsets are almost certainly where Launcher's own bootloader/partition
  table lived, so this flash likely overwrote Launcher on this device
  rather than coexisting with it — not confirmed, but worth knowing before
  assuming Launcher is still reachable without reflashing it separately.
  User confirmed the firmware booted with serial output afterward; that
  specific log wasn't pasted into this session (see the Launcher SD-drop
  capture below instead), so this closes the board-def checklist item on a
  verbal user report, a lighter bar than the fully-logged boots elsewhere
  in this doc.
- **2026-08-22** — Second Launcher SD-drop test (of a build now including
  the boot splash and auto-created config), full serial capture read line
  by line:
  - `[E][esp32-hal-spi.c:215] spiAttachMISO(): HSPI Does not have default
    pins on ESP32S3!` — benign noise, not a failure: `initDisplay()` calls
    `tftSPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS)`, explicitly
    passing `-1` for MISO since the display is write-only; ESP32-S3 has no
    real HSPI/VSPI (SPI is GPIO-matrix-routed on SPI2/SPI3 hosts), so the
    Arduino core's default-pin lookup for the `HSPI` enum has nothing valid
    to report and logs this as an `E`-level line even though nothing failed
    — boot continues immediately after it and the splash goes on to render.
    Confirmed by the boot proceeding normally in the same log, not by
    reading the core's source — worth doing that read if this ever needs
    silencing rather than just explaining.
  - `Antenna switch: P0 driven high.` then immediately three
    `sdCommand(): crc error` lines, then `GO_IDLE_STATE failed` and
    `f_mount failed: (3) The physical drive cannot work` — the SD card
    failed to mount this boot, unlike the first hardware boot's clean
    mount. `[config] No SD card detected (or mount failed) — using
    built-in default channel.` confirms `config.cpp` correctly took the
    fails-safe branch rather than crashing or reading garbage. This is the
    same symptom (SD-mount interference from the shared-I2C-bus device
    cluster) that bmorcelli/Launcher's own Cardputer-ADV notes already
    flagged as needing GPIO5 (`PIN_LORA_NSS`) driven high during early GPIO
    init — recorded in `board_pins.h` on 2026-08-12 but not acted on then,
    since the first hardware boot hadn't hit it. Applied now in `main.cpp`
    `setup()`, before any I2C or SPI access: `pinMode(PIN_LORA_NSS,
    OUTPUT); digitalWrite(PIN_LORA_NSS, HIGH);`. **Hypothesis, not
    confirmed** — needs a reflash and repeat boot(s) to know whether it
    actually fixes the mount reliability or the CRC errors were coincidence
    (marginal card seating, etc.).
  - User had edited `/loratrace/config.txt` to real override values ahead
    of this boot; since the mount failed before the file was ever opened,
    the override didn't apply (expected, given the above) **and** the
    user's edits were left completely untouched on the card — confirms
    `loadChannelConfigFromSD()`'s fails-safe design does what it's supposed
    to (never touches the file it can't successfully read), this was not a
    config-parsing bug.
  - Rest of the log matched the first hardware boot: `SX1262 initialized.`,
    default channel active, `Listening...`, `Free heap: 366756 bytes` (up
    from the earlier ~360KB range boot-to-boot, consistent with normal
    heap-layout jitter, not a leak signal on its own).
  - Two further findings from this same test round, not yet root-caused —
    see their own checklist/open-question entries above: the boot splash
    doesn't clear the whole physical panel (leaves visible remnants of
    Launcher's own screen), and interrupting Launcher's boot window showed
    its screen but didn't actually stop LoRaTrace RX from continuing to
    boot.
- **2026-08-22** — PR 4 (GPIO5/`PIN_LORA_NSS`-high-early fix + this doc's
  updates) merged. Next Launcher SD-drop boot's serial log, read line by
  line: Launcher's own pre-boot lines (`initialize TCA8418 at address
  0x34`, `SDCARD mounted successfully`, its app-menu enumeration listing
  both `porkchop-onepork.1-6` and `LoRaTraceRX-dev (2)`) confirm Launcher
  itself is still installed and functional on this device — resolves the
  earlier open question about whether the direct-USB flash a few tests ago
  might have overwritten it. Our own firmware then mounted the SD card
  cleanly with no CRC errors, and printed `[config] Applied channel
  override from /loratrace/config.txt` followed by `Active channel:
  918.500 MHz, SF8, BW125.0kHz, CR4/5` — the exact MeshOregon-style values
  the user configured at the start of this session. **This is the first
  end-to-end confirmation of the SD-config override path**, closing that
  checklist item. Per the user, though, the earlier mount failure "does
  that for every bin" regardless of firmware — so this clean run is a good
  sign for the GPIO5 fix but not proof the underlying SD reliability
  concern is fully gone; see the caveat on the PIN_SD_CS checklist item.
  Other lines matched expected/already-explained noise: `esp_core_dump_flash:
  Incorrect size of core dump image` (different garbage number than last
  time, -20771073 vs. the earlier 18023945 — consistent with "leftover
  flash bytes at that partition offset," not a new issue) and the
  `spiAttachMISO()` HSPI warning (already explained as benign). Free heap
  338660 bytes this boot — within the same rough range as prior boots
  (~338-367KB), consistent with normal heap-layout jitter rather than a
  leak.
- **2026-08-22** — Also added, confirmed with the user first since it's a
  further step past what the boot splash covered (its first genuinely
  `loop()`-updated content beyond the heartbeat dot, not just a one-shot
  `setup()` draw): a reserved "RX: none yet" splash line, overwritten in
  place with `len`/`rssi`/`snr` each time a packet decodes
  (`updateRxSplash()`). Lets the upcoming live-Meshtastic RX bench test be
  confirmed from the screen alone, no tethered laptop needed. Still one
  fixed line, no scrolling log, no keyboard/menus — the same "passive
  readout, not interactive UI" boundary the heartbeat dot and SD/heap
  status lines already sit on.

- **2026-08-22** — Root-caused and fixed the boot-splash screen-clear
  glitch, from a user-supplied photo showing Launcher's own boot graphic
  still filling the panel's lower portion behind LoRaTrace's splash text.
  Fetched and read `moononournation/Arduino_GFX`'s actual v1.4.0 source
  (`Arduino_TFT.cpp` `setRotation()`) rather than guessing at the offset
  constants: `Arduino_ST7789`'s two `(col,row)` offset-pair parameters are
  not "pair 1 for portrait rotations, pair 2 for landscape" as this repo's
  own `board_pins.h` comment previously claimed (written 2026-08-22 earlier
  the same day, based on how Launcher's confirmed-working config was read,
  not the library's own logic). The real `setRotation()` switch mixes one
  value from each pair per rotation; at rotation 1 (the only one this
  firmware uses), `_yStart` comes from `COL_OFFSET2` specifically —
  `main.cpp` only ever passed the "landscape" pair into the constructor's
  *first* slot, so `col_offset2`/`row_offset2` silently defaulted to 0,
  and `_yStart` came out 0 instead of the needed 53. `fillScreen()` then
  cleared a window that didn't line up with the panel's actual visible
  glass, leaving the mismatched region showing whatever was drawn there
  before (Launcher's graphic). Fix: pass all four offset constants
  (`TFT_COL_OFFSET_PORTRAIT`, `TFT_ROW_OFFSET_PORTRAIT`,
  `TFT_COL_OFFSET_LANDSCAPE`, `TFT_ROW_OFFSET_LANDSCAPE`, in that order) to
  the constructor in `main.cpp`; corrected the `board_pins.h` comment to
  document the library's real per-rotation mapping so this doesn't get
  re-broken later. The four numeric values themselves were already
  correct (they match the well-known offset table for this common ST7789
  135x240 IPS panel, e.g. as used in TTGO T-Display projects) — only the
  constructor call was wrong. Build-clean; **untested on hardware**, needs
  a reflash and a fresh photo to confirm.

- **2026-08-23** — First live `[RX]` lines on real hardware: three packets
  (`len=103 rssi=-28.00dBm snr=14.00dB`, `len=50 rssi=-58.00dBm
  snr=13.25dB`, `len=50 rssi=-28.00dBm snr=14.25dB`), captured on the
  user's own MeshOregon-style channel override (918.5MHz/SF8/BW125/CR4:5),
  not the hardcoded Meshtastic US default — closes the last fully-
  unverified Phase 1 checklist item (see above). Confirms the whole chain
  end-to-end on real hardware for the first time: antenna-switch path,
  FSPI wiring, DIO1 IRQ, and RadioLib's decode — and, since packets
  decoded with a clean CRC (no `[RX] read error` lines) and plausible SNR,
  implicitly confirms the sync word matches too, which CLAUDE.md flags as
  otherwise-unverified.
  - Immediate follow-up: the user sent 3 known test messages from a node
    physically next to the sniffer and only recognized 1 of the 3 logged
    lines as a match — "they just seem to be grabbing random messages."
    Two things are true at once here, and the investigation only
    confirms the first for certain:
    1. This is a promiscuous sniffer on a shared regional mesh channel by
       design (CLAUDE.md/DESIGN.md) — it will log other MeshOregon
       traffic (other nodes, relay/rebroadcast copies of the user's own
       messages) alongside the user's own sends, so a mismatch between
       "messages sent" and "lines logged" is partly expected, not
       automatically a bug. The -58dBm line among two -28dBm lines is the
       likely "someone else's/relayed packet" candidate, given the node
       under test was sitting right next to the sniffer.
    2. Root-caused a real gap in `main.cpp`'s `loop()`: `radio.startReceive()`
       to re-arm Rx Continuous ran *after* all the Serial/display I/O for
       the previous packet (multiple `Serial.print` calls plus a TFT
       `fillRect`/`print` splash update), not right after `readData()`.
       The SX1262 is out of Rx Continuous from the DIO1 IRQ until
       `startReceive()` runs again, so that whole print/draw window was a
       "deaf" period — a back-to-back packet (mesh relay traffic, or the
       user's own next test message arriving during that window) would be
       silently missed. Compounding it: the `len == 0 || len >
       sizeof(buf)` branch called `startReceive()` but never logged
       anything, so a dropped-for-bad-length packet left no trace at all,
       making "how many DIO1 events actually fired" unanswerable from the
       serial log alone.
  - Fix applied (`main.cpp` `loop()`): capture RSSI/SNR into locals and
    call `radio.startReceive()` immediately after `readData()` returns,
    *before* any Serial/display output — shrinks the deaf window to a
    couple of SPI transactions instead of the full print/draw path.
    RSSI/SNR still have to be read before the re-arm, not after:
    `GetPacketStatus` reports stats for the *last* packet, and a new one
    arriving right after `startReceive()` would overwrite them first.
    Also: the bad-length branch now logs `[RX] dropped, bad length N`
    instead of silently discarding, and successful decodes now print a
    hex dump of the payload so future correlation between "line in the
    log" and "message actually sent" doesn't have to rely on length/RSSI
    alone. This narrows the miss window; it does not eliminate it — a
    print- and draw-free path only comes with the Core-1 radio task from
    DESIGN.md §9 phase 2, which is explicitly out of scope for Phase 1.
    Build-clean by inspection (RadioLib/Arduino calls already used
    elsewhere in this file, no new APIs); **not yet bench-tested** — needs
    a reflash and a repeat of the 3-message bench test, ideally comparing
    payload hex across runs to tell "actually missed" apart from "that was
    someone else's packet" with more confidence than length/RSSI alone
    gave this session.

- **2026-08-23 (later same day)** — Reflashed with the re-arm fix above and
  re-ran the bench test: user reports "still kind of seeing a similar
  issue." So the late-re-arm timing was **a** real defect but **not the**
  root cause. Kept the fix (it's still correct, and Phase 2's radio task
  wants that ordering anyway) and went looking for the actual cause.
  - **Root cause found — wrong LoRa sync word, and this firmware never set
    one at all.** `main.cpp` called `radio.begin(freq, bw, sf, cr)` with
    only four arguments and never called `setSyncWord()`, so the sync word
    silently fell through to RadioLib's default. Verified in RadioLib's own
    `SX1262.h`: `begin(..., uint8_t syncWord = RADIOLIB_SX126X_SYNC_WORD_
    PRIVATE, ...)`, i.e. **0x12**. Verified separately in
    meshtastic/firmware `src/mesh/RadioLibInterface.h`: `const uint8_t
    syncWord = 0x2b;` — **0x2B**. The SX126x only raises an RX interrupt on
    a sync-word match, so the device was **structurally incapable of
    hearing current Meshtastic traffic**, while still happily decoding
    whatever unrelated LoRa gear sits on 0x12 (a common generic/hobbyist
    default). That is a precise, mechanical explanation for the reported
    symptom — "picks up random messages, misses my own node next to it" —
    and it explains why re-arm timing didn't move the needle.
  - Meshtastic's own source comment also **resolves DESIGN.md §7's "sources
    disagree" note** rather than just picking a side: *"For releases before
    1.2 we used 0x12 (or for very old loads 0x14). Note: do not use 0x34 -
    that is reserved for lorawan. We now use 0x2b."* 0x12 was **stale, not
    wrong** — pre-1.2 Meshtastic — which is exactly why credible-looking
    sources cite it. The separate "two-byte register mapping" ambiguity
    dissolves too: callers pass the one-byte value and RadioLib does the
    register mapping internally, and Meshtastic uses that same RadioLib
    API, so passing 0x2B matches it exactly. 0x34 → LoRaWAN independently
    corroborates DESIGN.md §6's fingerprint table.
  - Changes: added `sync_word` to `ChannelParams` and set Meshtastic's to
    a sourced `SYNC_WORD_MESHTASTIC = 0x2B` (`channel_plans.h`); passed it
    to `radio.begin()` and added it to the serial banner + splash
    (`main.cpp`); **left MeshCore explicitly on RadioLib's default with a
    TODO** rather than guessing, per CLAUDE.md. Added a `sync_word` key to
    the SD config (hex or decimal, `config.cpp`) specifically so the next
    bench test can A/B 0x2B vs 0x12 **without a reflash** — the thing this
    investigation kept wishing it had. Updated CLAUDE.md's house rule and
    DESIGN.md §6/§7 to match.
  - Verification: `pio` isn't available in this environment, so the native
    test suite was compiled and run directly against upstream Unity
    (g++, host) — **5/5 pass**, including two new ones pinning Meshtastic
    to 0x2B (and explicitly *not* 0x12/0x34) and asserting MeshCore is
    still on the default, so that test fails loudly the day someone
    resolves it. `channel_plans.h` also compiles clean under
    `-Wall -Wextra`. The firmware build itself (`pio run`) is **not**
    verified here — CI covers it.
  - **Still unproven on hardware:** that 0x2B actually recovers Meshtastic
    RX. The mechanism is solid and the values are source-verified, but
    until a reflash logs packets that correlate with deliberately sent
    messages, this is a very well-founded hypothesis, not a confirmed fix.

- **2026-08-23 (hardware) — SYNC-WORD FIX CONFIRMED. Phase 1 RX is real.**
  User pulled the branch, flashed, and captured 9 packets. Banner reads
  `918.500 MHz, SF8, BW125.0kHz, CR4/5, sync 0x2B`. This closes the
  hypothesis from the previous entry: 0x2B recovers Meshtastic RX.
  - **The payload hex proves these are genuinely Meshtastic frames**, not
    "some LoRa traffic." Decoded against Meshtastic's own `PacketHeader`
    (`src/mesh/RadioInterface.h`: `to`,`from`,`id` as 4-byte LE each, then
    `flags`,`channel`,`next_hop`,`relay_node`): every packet has a
    well-formed header, `to == 0xFFFFFFFF` (broadcast) on 8 of 9, a
    consistent channel hash `0xF7` across all of them, and `hop_limit` /
    `hop_start` in the flags byte that make sense (start 7). Random noise
    or another protocol would not produce that structure.
  - **It also retroactively explains the whole "random messages" mystery.**
    Packets arrive in *pairs* — same `from`, same packet `id`, same
    ciphertext, but `hop_limit` 7 then 6, a different `relay_node`, and a
    very different RSSI:

    | from | id | hop | relay | rssi |
    |---|---|---|---|---|
    | 0x1BBF065C | 0x2C618F2D | 7 | 0x5C | -60 |
    | 0x1BBF065C | 0x2C618F2D | 6 | 0x6A | -26 |

    That's the original transmission followed by a **mesh rebroadcast**.
    Two nodes are visible — `0x1BBF065C` (relay byte 0x5C, arriving
    -57..-61 dBm, i.e. distant) and `0x82D7776A` (relay byte 0x6A, arriving
    -16..-26 dBm, i.e. the node next to the sniffer) — and each relays the
    other's broadcasts. So a single sent message legitimately produces
    ~2 log lines with wildly different RSSI. The earlier "-58 among two
    -28s" that looked like someone else's traffic was almost certainly this
    same original/relay pairing. **A detection is not a message** — worth
    carrying into the §8 log schema and §6 fingerprinting: dedupe by
    (`from`,`id`) if a "unique messages heard" count is ever wanted, and
    `relay_node`/`hop_limit` are free topology data a wardriver should keep.
  - One packet is a **unicast**, not a broadcast: `to=0x82D7776A`
    `from=0x3B9292F1`, len 95 — a third node sending directly to the near
    node. Confirms the sniffer sees DMs, not just broadcasts (they're
    encrypted, but their *existence* and routing metadata are visible).
  - **Heap: no leak.** 338496 bytes at boot, still ~338496 after sustained
    RX. Closes the "real `ESP.getFreeHeap()` under load" open question for
    Phase 1's purposes — the number is stable under active receive, not
    just at idle. Phase 2 will need re-measuring once tasks/queues/SD
    buffers exist, but the no-PSRAM risk calls in ROADMAP.md now rest on a
    measured number instead of a paper estimate.
- **2026-08-23** — Added `src/gps_probe.cpp` + `[env:gps-probe]`: a
  standalone GPS bring-up sketch (raw NMEA passthrough, sentence counter,
  fix detection, and a 5-second heartbeat that distinguishes "no bytes at
  all" from "bytes but no fix"). Deliberately a **separate build env**, not
  a few lines in `main.cpp` — the sync-word bug is the argument for it:
  radio RX stayed broken for days partly because it was entangled with
  everything else booting, and nothing in this probe can perturb the
  now-working RX firmware because it isn't compiled into that build. Note
  `[env:cardputer-adv]` gained a `build_src_filter` excluding the probe;
  without it the two `setup()`/`loop()` pairs collide at link time. The
  NMEA field parser is hand-rolled (no TinyGPS++) so the probe can't fail
  because of a library — unit-tested on the host against real GGA/RMC
  samples under ASan+UBSan, including no-fix sentences, `*`-terminated
  fields, and out-of-range indices.

- **2026-08-23 — GPS probe found two faults, one of them self-inflicted.**
  The probe reported zero UART bytes. That was a *useful* zero: a baud
  mismatch produces garbage bytes, so silence ruled baud out immediately and
  pointed at power or pins.
  - **Primary cause: the probe never powered the GPS.** PI4IOE5V6408 P0
    doesn't only switch the RF antenna path — M5Stack's own Arduino example
    for this Cap drives expander pin 0 high to enable **GPS power**, and the
    LoRa868 Cap (which has no GPS) omits the call entirely. `main.cpp` set
    P0; the probe didn't, precisely *because* it had been written to isolate
    itself from the rest of the boot sequence. The isolation instinct was
    right; including the power rail in it was not. Extracted to
    `io_expander.cpp` and shared by both binaries so they can't drift again.
  - **Secondary: M5Stack contradicts itself on GPS RX/TX polarity.** Their
    docs PinMap reads `GPS_TX -> G13` (host receives on G13, what
    `board_pins.h` had); their tutorial's working example says
    `RXPin = 15`. Empirically G13 saw nothing. Rather than pick one and make
    an operator reflash to test the other, the probe now **A/Bs both
    orderings every 8s** and prints which produces bytes. `board_pins.h`
    adopts the example-code ordering and records the contradiction, since
    this project has already been burned once by a scraped pin table.
  - Probe also now shares `nmea.h`/`gps_parse.h` with the GPS task, so it
    validates checksums and reports real fix data instead of carrying a
    private parser — and exercises the same code the firmware depends on.
- **2026-08-23 — Phase 2 built.** Three tasks per DESIGN.md §2, a 32-deep
  cross-core queue of ~36B records, and the shared-bus arbitration that
  DESIGN.md §7 flagged as an open question before Phase 2 could start:
  - **`spi_bus.h/.cpp`** now owns both the `SPIClass` and a FreeRTOS
    **mutex** (not a binary semaphore — priority inheritance is the point:
    without it the radio task can block behind a mid-priority task that
    isn't even using SPI, and on this board that means lost packets).
  - **`radio_task`** (Core 1, prio 3) does the whole SX1262 transaction
    inside one short critical section, reading RSSI/SNR and re-arming RX
    *before* releasing — carrying forward the Phase 1 ordering fix. If the
    queue is full it drops and counts rather than waiting: a receiver that
    stalls to preserve a log row is strictly worse than one that misses it.
  - **`logger_task`** (Core 0, prio 2) batches rows into ~2KB and writes
    open/append/close per flush. Sizing rationale: the goal is **short**
    flushes, not rare ones. The SX1262 FIFO gives roughly one packet-time of
    slack, so many small bus holds beat occasional large ones — which is
    also why the buffer is 2KB and not 32KB. Open-per-flush because this
    device gets turned off by being unplugged.
  - **`gps_task`** (Core 0, prio 1) parses into a local and publishes under
    a mutex, keeping the critical section to one struct copy.
  - A stale GPS fix is treated as no fix (10s max age): at driving speed
    that's already ~250m of error, and an empty coordinate is more honest
    than a confidently wrong one. Same reasoning as refusing to render a
    no-fix as `0,0`.
  - Everything decision-shaped lives in pure headers (`detection.h`,
    `nmea.h`, `gps_parse.h`) so **26 host tests** cover it. The
    `test_detection` fixtures are the real packets captured off-air on
    2026-08-23, including an original/rebroadcast pair — so a regression in
    header parsing produces a concrete wrong answer about real traffic
    rather than an abstract failure.

- **2026-08-23 — GPS UART alive; probe reworked from firehose to instrument.**
  The A/B settled M5Stack's documentation contradiction on the first try
  (RX=G15), and the P0 power fix was the unlock. But the first successful
  run dumped ~300 lines of raw NMEA, burying the two things that actually
  mattered: `ANTENNA OPEN` and `00` satellites in view. Fixed:
  - Raw passthrough is now **time-boxed to 3s** — proof of life, not a
    monitoring mode. At ~80 sentences/5s across five constellations it's
    unreadable as a steady state.
  - **`$GxTXT` messages are surfaced on change**, with an inline note about
    the passive-antenna false positive so the next person doesn't chase it.
  - **Satellite visibility is parsed from GSV** (field 3) per constellation
    and GSA fix type (field 2). This is the diagnosis that matters once the
    UART is proven: **0 in view everywhere = sky/antenna; some in view but
    no fix = just needs time** for almanac/ephemeris. The status line now
    reads e.g. `sats=0 (GP:0 GL:0 GA:0 BD:0 GQ:0) fixtype=1`, and the advice
    branches on it instead of guessing.
  - Parsing verified on the host against the operator's actual captured
    sentences.
- **2026-08-23 — Fixed a latent static-initialisation-order bug** found by
  re-reading the Phase 2 diff adversarially rather than by any test.
  `radio_task.cpp` builds its `SX1262` at namespace scope via
  `new Module(..., sharedSpi())`, which runs during static init; `sharedSpi()`
  returned a reference to a namespace-scope global in a *different*
  translation unit, and C++ guarantees nothing about cross-TU static init
  order. It compiled and would have worked by luck (Module only stores the
  pointer, and nothing touches the bus until `setup()`), which is precisely
  what makes this class of bug expensive later. Now a function-local static,
  which is guaranteed constructed on first use. Worth noting CI could never
  have caught this — it is not a compile error.

- **2026-08-23 — UI / battery / keyboard confirmed on hardware, first try.**
  Everything sourced in the previous entry proved correct on the bench:
  - **Keyboard paging works**, which specifically validates the TCA8418
    wake sequence. That part boots in SLEEP and reports nothing until
    configured, so "keys work" is real evidence the `begin()` +
    `matrix(7,8)` path (taken from Launcher's working ADV interface) is
    right — not just that I2C is alive.
  - **Battery validated, not merely sourced**: 4.09V on USB, 3.76V / 58%
    on battery. 58% is exactly `(3765-3300)*100/800`, and both voltages sit
    in real LiPo range — a wrong divider would have produced ~2.0V or
    ~8.2V. GPIO10 and ratio 2.0 from M5Unified's `board_M5CardputerADV`
    case are correct for this board.
  - Worth recording so it isn't mistaken for a calibration bug later: on
    USB the pack reads ~4.09V, so the gauge sits just below 100% while
    charging. That's the hardware, not the maths — and since M5Stack's docs
    say charge *status* is unreadable on this board, there is no way to
    display "charging" instead.
  - **`ANTENNA OPEN` is confirmed benign.** The GPS reached "acquiring"
    (satellites in view > 0) while still emitting that message, which is
    direct evidence the antenna works. Upgrades the earlier reasoned
    MEDIUM-HIGH assessment to confirmed: the supervisor senses DC bias
    current that only an active antenna draws, and this Cap's is passive.
  - Net: of the two things flagged as unverifiable without hardware
    (battery calibration, keyboard wake), both came back correct on the
    first flash — the payoff for sourcing them from M5Unified and Launcher
    rather than guessing.

- **2026-08-23 (hardware) — GPS FIX ACQUIRED, 14 satellites.** The last
  never-proven piece of Phase 2 hardware now works end to end: expander P0
  powers the module, RX=G15 carries the NMEA, the parser validates it, and
  the receiver resolves a position. Every step of that chain had been
  reasoned about or tested in isolation; this is the first time all of them
  ran together and produced a coordinate.
  - Worth noting *which* diagnosis paid off: the probe's `sats_in_view`
    branch. "Some in view but no fix = just needs time" was the advice it
    printed, and that turned out to be exactly the situation — no further
    code change was needed between the last entry and the fix landing. The
    instrument was right, and the fix cost patience rather than debugging.
  - This closes the last **blocking** unknown for Phase 2. What remains is
    not a bug hunt but the exit criterion itself: an unattended multi-hour
    run whose logs come back clean.

- **2026-08-23 — Session health log, so an unattended run records itself.**
  With GPS working, the next gate is ROADMAP.md's Phase 2 exit criterion:
  "no dropped packets attributable to SD latency, no crash from heap
  exhaustion over a multi-hour run." Reading the firmware against that
  sentence turned up an awkward gap — **every number that settles the claim
  existed only where nobody would be looking.** `radioQueueDropCount()`,
  `loggerMaxFlushMs()`, `spiBusContentionCount()` and the heap were exposed
  precisely *because* the criterion demanded them, but they surfaced only in
  the serial status line and the on-screen pages. Both need a human present.
  A device on battery, driven for three hours and unplugged at the end,
  produced a detection log and no evidence whatsoever about its own health.
  The one run the criterion actually describes was the one run whose result
  could not be read.
  - Fix: `/loratrace/session.csv`, one row a minute plus a `reason=boot`
    row, schema in DESIGN.md §8.2, formatting in the pure header
    `session_log.h` with **8 host tests**. ~180 rows over a three-hour
    drive.
  - **`heap_min` (`ESP.getMinFreeHeap()`), not just the sample.** A
    once-a-minute reading of free heap can walk straight past a transient
    trough, and the trough is what actually ends a long run. Same reasoning
    applies to the serial line and the SYSTEM page, so both now show it too.
  - **Boot rows exist because sessions append to one file.** Without a
    marker, a power cycle mid-drive reads as the counters spontaneously
    resetting — which looks like a firmware fault instead of someone
    catching the USB cable with their knee.
  - Also records **time-to-first-fix** (`gpsFirstFixMillis()`). It is an
    operational number for a wardriver — how long after switching on the
    track becomes usable — and it can only be measured across a whole
    session, never reconstructed afterwards.
  - Deliberately *not* a second detection stream: this is instrumentation
    and it is best-effort. A lost health row gets zero retries, and the
    write is skipped entirely when SD is down, because instrumentation must
    never be the thing that ends the run it is measuring.

- **2026-08-23 — Fixed a mid-session SD remount that could never have
  worked.** Found by re-reading the logger against the "card reseated
  mid-drive" story it claims to support, not by any test. The retry path
  called `SD.begin()` — but the ESP32 Arduino core's `SD.begin()` opens with
  `if(_card) return true;`. Once a card has been mounted and then pulled,
  `_card` is still non-null, so every retry "succeeded" instantly while
  every subsequent write kept failing: `sdReady` would flip back to true,
  the next flush would fail, and the loop would spin that way forever. The
  card would never come back without a reboot, which is precisely the
  scenario the retry exists to handle.
  - `openLogsLocked()` now calls `SD.end()` first, unconditionally, so the
    remount is real. Unconditionally rather than only-on-retry so this
    function is the sole authority on the mount regardless of what the
    boot-time config read (`config.cpp`, which mounts and never unmounts)
    left behind.
  - Same species as the static-init-order bug from the Phase 2 diff: it
    compiles, it runs, and it is wrong only in the failure path — so
    neither CI nor a healthy bench session can see it. This one is worse,
    though, because the failure path is the *documented feature*.
  - The logger's two SD writers now share one `appendToFile()` helper with
    an explicit `WriteResult` (`OK` / `BUS_BUSY` / `FILE_ERROR`), since
    "bus busy, retry" and "card gone, give up" were already distinct
    behaviours that a bool return had been quietly flattening. Every bus
    hold the task takes goes through that one function, which keeps
    `maxFlushMs` honest as "the worst hold the logger caused" now that
    there is more than one file being written.


- **2026-08-23 — `rx_uptime_ms` added to `detections.csv`; the field was
  being captured and then thrown away.** Came out of asking a plain question
  of the new health log — "does this survive across runs, and is anything
  timestamped?" Both files turned out fine on persistence (append-only,
  header written only when absent, nothing truncates), but checking how a
  row gets placed in time exposed the gap: `Detection::rx_millis` is stamped
  by the radio task, crosses the queue, and carries a comment saying it
  "lets post-processing spot a stale GPS stamp caused by queue backlog" —
  and `detectionFormatCsv()` never wrote it. It reached the logger and died
  there.
  - Why it matters more than it sounds: a detection heard **before the first
    GPS fix** has an empty `timestamp_utc` *and* empty lat/lon. With uptime
    dropped, such a row had no time reference of any kind — not orderable,
    not joinable to `session.csv`, not even "40 seconds in". On a cold start
    that is every packet heard during TTFF.
  - Appended as the last column so existing parsers keep working, same rule
    as `logger_stack_free` in the session schema.
  - Absolute time comes from GPS because there is no verified RTC on this
    board (no RTC part is referenced anywhere in this project — worth
    sourcing before anyone assumes one exists). DESIGN.md §8.3 now writes
    down the consequence and the arithmetic that recovers wall-clock for a
    whole run from any single timestamped row: `timestamp_utc - uptime_s`.

- **2026-08-23 — One wardrive, one directory: `/loratrace/runNNNN/`.**
  Operator's call, and the right moment for it — changing the log layout
  *after* the Phase 2 validation drive would have invalidated that run.
  Previously both logs were single files every power-on appended to. Durable,
  but not usable: a drive is the unit an operator thinks in, and one
  continuous file turns share/import/delete/diff into text-editing chores.
  - **Indexed, not timestamped, and this is the interesting constraint.**
    The name must be chosen the moment logging starts — and at that moment
    the device does not know the time. Absolute time comes from GPS, there
    is no verified RTC on this board, and a cold TTFF is tens of seconds.
    Timestamp naming would mean either delaying file creation (losing every
    packet heard during TTFF — exactly the rows the `rx_uptime_ms` fix was
    added to rescue) or renaming later (leaving a provisional name behind on
    any power cut before the rename). An index needs no clock and is stable
    under power loss. The wall clock still reaches the card, recorded inside
    the run on the first health row with a fix.
  - **Next index comes from scanning the card, not a counter file.** The
    directory listing is the truth: it cannot drift out of sync, and there
    is no mutable state to corrupt on a power cut mid-write. Scanning for
    the *highest* index rather than the first gap means a deleted run's
    number is never silently reused by a later one.
  - **Parsing is strict — `runNNNN`, exactly four digits, nothing after.**
    That strictness is load-bearing in both directions and is what most of
    the 8 new host tests cover: too loose and `config.txt` or a stray file
    bumps the index; too strict and a real run is missed, so the next run
    reuses its number and appends into someone else's drive.
  - A card **reseated mid-drive rejoins the same run** (resolved once per
    power-on, not per mount) rather than splitting one drive across two
    folders. The gap is still recorded honestly, as `sd` going down and back
    in that run's own health rows.
  - Added a **`run` column to both CSVs**. Redundant with the directory a
    file sits in, right up until several runs are concatenated for analysis
    — at which point every row's uptime has restarted at zero and the merged
    data is silently ambiguous about which drive a row came from.
  - The run number shows on the RADIO page as `r<N>`: an operator about to
    set off wants to know the folder their data is landing in, and it is the
    one thing on that page they cannot infer from anything else.
  - **Deliberately not built yet:** an explicit start/stop gate. Today a run
    is a power-on because that is the only gate the firmware has. The
    rollover is a single internal step, so a Phase 6 UI control (or a
    profile switch) can start a new run without reshaping any of this.
  - Legacy top-level `detections.csv`/`session.csv` from earlier firmware
    are left alone on existing cards; the scan skips them by construction.

- **2026-08-23 (hardware) — v0.2.2 first boot: per-run directories work,
  and the flush metric was quietly lying.** Operator's serial capture,
  read line by line.
  - **`run=2` is the confirmation that matters.** The board had been reset
    once (the log shows two banners either side of a
    `rst:0x15 (USB_UART_CHIP_RESET)`), so the first boot created `run0001`
    and the second correctly scanned the card, found it, and claimed
    `run0002`. That exercises the whole path on real hardware: directory
    listing, `File::name()` shape, strict `runNNNN` parsing, mkdir. It also
    proves the scan **skipped `config.txt`** rather than counting it — the
    override applied on the same boot, so the file was definitely sitting
    there in the same directory being listed.
  - **`health=1 sd=ok`**: the boot health row reached the card.
  - **`flushes=0 maxflush=26ms` exposed a real defect in my own change.**
    Zero detection flushes had happened; the 26ms was the boot health row,
    charged to the detection-flush metric because both writers shared one
    high-water mark. I had written the comment claiming that was correct
    ("the worst hold the logger has caused, whichever file caused it") and
    it is — for one of the two questions this number gets asked. "Is the
    logger starving the radio?" is the max across both writers. "Is my
    batch sizing wrong?" is only ever about detection flushes. Merging them
    answered the first and silently destroyed the second, which is worse
    than useless on a metric that exists to tune the batch buffer. Now
    tracked per writer: `max_flush_ms` and `max_session_ms`, both in the
    health row and the status line.
    - Worth noting the shape of the mistake: not a crash, not a wrong
      value, but a *correct number answering the wrong question*. No test
      could have caught it, and it took one real boot printing two numbers
      side by side to make it obvious.
    - The 26ms itself is useful data, not noise: that is the first write to
      a freshly mounted card, and it is the current worst-case bus hold on
      record.
  - **`[E] spiAttachMISO(): HSPI Does not have default pins on ESP32S3!` is
    benign and is now documented in `board_pins.h` instead of being fixed.**
    The display is write-only so its bus is begun with `MISO = -1`; the core
    reads negative as "use this host's default MISO", finds S3 has none for
    HSPI, logs at ERROR and returns without attaching — which is precisely
    the desired outcome. The only way to silence it is to hand the bus a
    real GPIO as MISO, i.e. to claim a pin for a purpose it does not serve.
    A misleading pin map is a far worse legacy than a noisy boot log, and
    this project has already paid for one of those. Same category as the GPS
    `ANTENNA OPEN`: loud, alarming, correct to ignore.
  - `[W] Wire.cpp begin(): Bus already started` x2 is the documented
    deliberate re-`begin()` in `uiTaskStart()` plus the TCA8418 library
    doing the same. Harmless.
  - **Heap moved and it is worth writing down:** 317676 free after task
    start, 313068 at the first status line, `heapmin=308488`. Phase 1's
    idle number was ~338KB, so the three tasks, the queue, the UI and the
    SD buffers cost roughly 21-25KB together — comfortably inside the
    no-PSRAM budget, and the ~4.5KB gap between `heap` and `heapmin`
    already shows the trough tracking is doing its job.
  - **Still unproven, and only time fixes it:** `rx=0` and `fix=none` at the
    first status print, seconds after boot. `nmea=64 badcrc=0` says the GPS
    is talking cleanly and just hasn't fixed yet.
  - **Known wart, deliberately not papered over:** attaching a serial
    monitor toggles DTR and resets the board, so every bench session claims
    a fresh run folder holding one health row and no detections. Honest
    consequence of "a run is a power-on"; trivially identifiable and a few
    hundred bytes each. A Phase 6 start/stop gate is what actually fixes it.

- **2026-08-23 (hardware) — run0005 capture: the layout is confirmed, and
  the data exposed two more defects.** Operator pulled
  `/loratrace/run0005/` off the card. Both files present, both headers
  correct, `run=5` consistent across them, `rx_uptime_ms` populated
  (115175, 120944), two genuine Meshtastic detections
  (`!3e0c868b` -67dBm, `!69858668` -62dBm). `queue_drop`, `row_drop`,
  `bus_miss` and `bus_contention` all 0. `rows=2 flushes=2` matches `rx=2`.
  **The per-run layout, both schemas, and the whole logging path are now
  hardware-confirmed.**
  - **`logger_stack_free` paid for itself immediately.** It reports 1432
    bytes free at its worst, i.e. ~3688 used of the 5120 stack. The bump
    from 4096 was made on reasoning alone and would have left roughly 400
    bytes of headroom — uncomfortably thin under SD/FatFS. That is no
    longer an argument; it is a measurement, which was the entire point of
    logging it.
  - **Defect 1: file timestamps read 1980.** Nothing ever set the system
    clock, so it sat at the epoch and FAT stamped every file with its own
    1980 floor. A card full of runs could not be ordered by anything but
    its contents. `gps_task` now adopts GPS UTC once per power-on via
    `settimeofday()`, gated on **time alone rather than a position fix** —
    GPS has the time long before it has a fix, and this capture is the
    proof: `timestamp_utc` is populated from 15:12:29 onward while `lat`,
    `lon`, `sats` and `fix_type` all still say no fix. Waiting for position
    would have left the files wrong for that entire window.
    - Conversion is `gpsFixToEpoch()` in `gps_parse.h`, using Hinnant's
      days_from_civil rather than `mktime()`/`timegm()`: `mktime()`
      interprets its input in the process timezone and `timegm()`'s
      availability varies by libc, and the correct answer must not depend
      on whether something called `tzset()` first. Pure arithmetic, host
      tested against known timestamps including a 2028 leap day and the
      2100 non-leap-year case.
    - A plausibility floor (`GPS_MIN_PLAUSIBLE_YEAR = 2024`) gates it. This
      value stamps every file on the card, so a garbled sentence that got
      past the checksum and yielded year 2000 would silently backdate a
      whole run.
    - Known limits, documented in DESIGN.md §8.3 rather than hidden: files
      *created* before the clock is set keep their 1980 creation date (the
      run directory and both CSVs, created at mount); mtime corrects on the
      first append afterwards. A run where GPS never supplies a date stays
      1980 throughout — inherent without an RTC.
  - **Defect 2: the health log recorded the useless satellite count.**
    Every row reads `sats=0 fix_type=1`, which cannot distinguish "twelve
    satellites in view, still acquiring" from "antenna disconnected". This
    project already learned that lesson explicitly — `gps_parse.h` carries
    a comment about it, and the GPS probe was rewritten around it on
    2026-08-23 — and then the session log, the file whose whole job is
    explaining a run after the fact, shipped with only the used count.
    `sats_in_view` added alongside. The probe knew better than the logger
    did, which is a good argument for reading old lessons before writing
    new files.
  - **Watch item, not yet a problem:** `nmea_bad_crc` climbs 0 → 2 → 8
    against `nmea` 16 → 962 → 1885, about 0.4%. Low enough to be ordinary
    UART noise; worth a second look if it scales with detection traffic,
    which would point at bus or interrupt contention rather than the wire.

- **2026-08-23 (hardware) — v0.2.4, 5-minute monitored run: clock fix and
  metric split both confirmed; two things flagged for follow-up, neither
  blocking.**
  - **Clock-from-time-alone confirmed exactly as designed.**
    `[gps] system clock set from GPS: 2026-8-23 15:24 UTC` fires while the
    status line right before AND after it still reads `fix=none` — the
    clock was set with no position fix ever having landed this run. That is
    precisely the scenario the fix exists for (GPS has time long before it
    has a fix) and this is the first hardware evidence it works.
  - **`maxflush`/`maxhealth` split holds up.** `maxflush=0ms` while
    `flushes=0`, both climb together and independently afterward
    (`maxflush` 27→29ms tracking detection flushes, `maxhealth` 26→40ms
    tracking health rows on its own clock) — no more of the "flushes=0
    maxflush=26ms" contradiction from the previous boot.
  - **Zero `qdrop`/`busmiss` for the full 5 minutes**, heap flat at
    312796/308204 after initial settling, `rows`/`flushes` batching
    correctly (8 detections in 6 flushes). No regressions.
  - **Detections arrived as a burst**: `rx` 2→4→6→8 across three consecutive
    5s ticks, consistent with the original/rebroadcast pairing documented
    from the Phase 1 capture.
  - **No fix the entire run** (`fix=none` throughout, `sats` never used).
    Almost certainly bench/indoor conditions — an operator monitoring
    serial in real time is not driving. Flagged rather than assumed: this
    run does not, by itself, exercise the "GPS fix acquired" leg of
    ROADMAP.md's Phase 2 exit criterion. That still needs an outdoor run.
  - **`nmea_bad_crc` rate roughly doubled during the detection burst.**
    Baseline is ~0.5-0.6% of sentences (nmea 80→1850, badcrc 0→10). During
    the burst window, while `flushes` climbed 2→6 (nmea 2163→2900), badcrc
    went 11→20 — about 1.2%, roughly 2x baseline — then settled back to
    ~0.46% afterward (2900→5070). Small and non-blocking (GPS never had a
    fix to lose this run regardless), but a real, quantified correlation
    between active SD-flush bus activity and corrupted NMEA sentences,
    worth watching once a live wardrive has both GPS fixes and steady
    detection traffic at the same time — that is the condition this run
    couldn't test.
  - **Resolved: `run=5` on a card the operator confirms was clear of every
    run folder.** Not a scan bug — the explanation is the DTR-reset wart
    already on record. `esptool` toggles DTR/RTS around the flash itself,
    and reopening `pio device monitor` (or any terminal reconnect) does the
    same; each is a full power-on as far as `loggerTaskStart()` is
    concerned, and each claims the next index. Runs 1-4 are almost
    certainly boot-only stubs (`reason=boot`, `rx=0`, no detections) from
    flashing and reconnecting the monitor before settling in to watch the
    session that became run 5. Not verified by directly listing the card,
    but consistent with every other number in this capture and with the
    operator's own account.
    - Operator has decided to accept this rather than build the start/stop
      gate now ("let them stack") — matches what the PR already scoped as
      deliberately deferred to Phase 6. A card that fills with mostly-empty
      run folders over a bench session is the known cost of that choice; a
      few hundred bytes each, and `rx=0` makes them trivially filterable
      later if that ever matters.

- **2026-08-23 (hardware) — first outdoor, battery-only run: GPS fix closed,
  `maxflush` measured, first combined-load data point.** Run 6, ~37 minutes
  on the operator's deck, no serial console (judged entirely from the two
  CSVs the exit criterion was designed to make readable after the fact).
  - **GPS reached a 3D fix almost immediately** (`ttff_s=45`) and held it the
    entire run, sats climbing 9→22. This is the first time this board has
    ever produced a fix outdoors under real conditions — closes that
    checklist item and retires the lingering antenna suspicion from the
    indoor `00`-sats capture.
  - **`max_flush_ms` peaked at 29ms** across 18 flushes — the first real
    measurement of this number, closing that checklist item. Nowhere close
    to a level that would starve the radio; `BATCH_BUF_SIZE` doesn't need
    revisiting.
  - **Every drop counter stayed at 0 for the full run**: `crc_err`,
    `queue_drop`, `bus_miss`, `row_drop`. `sd=ok` and `bus_contention=0`
    throughout. `heap_free`/`heap_min` settled flat (312596B / 308004B)
    after the first two minutes with no further decline — no leak signal.
  - **All 19 detections carry a fresh, plausible fix** clustered tightly
    around the deck's coordinates, matching the concurrent `session.csv`
    positions row for row — the lat/lon-correctness leg of the exit
    criterion reads clean on this run.
  - **First time GPS fix + steady detection traffic coincided**, which is
    the exact combined condition the run0005 and v0.2.4 entries above
    flagged as untested for the `nmea_bad_crc`/bus-contention question.
    Answer: **~2.0%** sustained for the whole run (765/37707), above the
    ~0.4-0.6% baseline and ~1.2% burst rate seen separately before. Same
    direction as those findings, not a new failure mode, and fix quality
    never wavered — but the highest number recorded yet, so it's worth a
    closer look (a scoped burst-correlation check, not necessarily a fix)
    before Phase 3 adds a second profile's worth of traffic on top.
  - Battery 3812mV→3740mV (-72mV) over 37 minutes — a rate that leaves
    comfortable headroom over several hours, for whenever the actual
    multi-hour run happens.
  - **Not yet the exit criterion itself**: 37 minutes stationary on a deck
    is a strong, clean data point, but ROADMAP.md's Phase 2 gate is
    specifically a *multi-hour* unattended run, and this doesn't reach
    that bar on duration or on being a real drive (GPS never had to
    track movement). Phase 2 stays open pending that run.

- **2026-08-23 — v0.2.5: re-analyzed run 6 minute-by-minute, and the
  simple "SD flushes cause the noise" story doesn't hold up as cleanly as
  the earlier short bench sessions suggested; added instrumentation to
  test the actual mechanism instead of continuing to infer it.**
  - **The re-analysis.** Bucketed `session.csv` into its 37 one-minute
    intervals and split them by whether a detection flush landed in that
    minute. Windows *with* a flush: 304/13248 bad (2.29%). Windows
    *without* one: 461/24459 bad (1.88%) — a real but modest ~20% relative
    bump, not the ~2x jump the shorter v0.2.4/run0005 sessions hinted at.
    More telling: the very first three minutes, **before any flush had
    ever happened this run**, were already running 1.25-1.86% bad — close
    to this run's own "quiet" baseline and already above the ~0.4-0.6%
    baseline those earlier sessions established. Two readings of that:
    either this run's baseline noise floor is just higher for an unrelated
    reason (battery power outdoors vs. USB-tethered bench, different RF
    environment), or the once-a-minute health-row write — which fires in
    *every* interval, flush or not, so it can't be isolated by this kind
    of bucketing — is itself already enough to account for most of the
    baseline. The 60-second granularity in `session.csv` can't tell these
    apart; deciding between them needs a direct measurement, not another
    correlation on the same coarse data.
  - **Added `gps_max_loop_gap_ms`**: the worst gap the GPS task (Core 0,
    lowest priority by design — DESIGN.md §2) ever went between passes of
    its UART-drain loop. This is the actual mechanism the bus-contention
    theory depends on (a busy logger starving the GPS task long enough for
    the UART ring buffer to overflow) measured directly, rather than
    inferred from `nmea_bad_crc` moving around. If this stays small (a few
    ms) through the whole 2-hour run even during flush-heavy stretches,
    the CPU-starvation theory is wrong and the noise is coming from
    somewhere else — wiring, RF coupling off the LoRa front-end, or the
    module itself. If it spikes into the hundreds of ms alongside SD
    activity, that's the theory confirmed.
  - **Added `gps_oversize_drops`**: the line-assembly buffer (96 bytes,
    NMEA's own spec limit) has always silently discarded and resynced on
    overrun, with zero counter anywhere. A dropped byte that happens to be
    a sentence's own `\n`/`\r` merges two sentences into one, overruns the
    buffer, and vanishes without ever touching `nmea_bad_crc` — so the true
    corruption rate could be higher than that counter alone has ever shown.
  - **Bumped the GPS UART ring buffer 256B → 1024B** (`gpsSerial.
    setRxBufferSize(1024)`, before `.begin()`). At ~17 sentences/sec x
    ~75 bytes measured this run, 256B is under 200ms of slack before an
    unread buffer starts dropping bytes — cheap insurance regardless of
    what the loop-gap measurement shows, and a natural A/B against this
    run's ~2.0% number: if the rate drops substantially on the next run,
    that's independent evidence for the same starvation theory.
  - Both new fields appended after `run`, matching this schema's own
    append-at-the-end convention (`rx_uptime_ms`/`logger_stack_free` set
    the precedent) — existing tooling that reads earlier columns by
    position is unaffected. `DESIGN.md` §8.2 and `test/test_session_log/`
    updated to match; two new host tests cover the new columns' formatting
    and position, all 11 tests (54 across the full native suite) still
    pass. No `pio` in this environment — verified by compiling the native
    suite directly against upstream Unity (g++, host), same workaround
    used for the sync-word fix.
  - **Not a fix — deliberately.** Nothing about *behavior* changed except
    the buffer size; this is purely "make the next run answer the question
    the last one couldn't." The right next step is the already-planned
    2-hour deck run on this build, then reading `gps_max_loop_gap_ms`
    first, before `nmea_bad_crc` itself.

- **2026-08-23 (later same day)** — The planned run happened: run0007,
  v0.2.5, 2h30m unattended on the deck. Closes the Phase 2 multi-hour exit
  criterion and the `nmea_bad_crc` watch item — see the checklist entry
  above for the numbers. Reading `detections.csv` (110 rows) surfaced one
  real finding, worth recording in detail since it changed conclusions
  mid-investigation rather than landing on the first theory:
  - **The observation.** Grouping detections by `channel_or_node_id` and
    pairing consecutive hits from the same id within 15s of each other
    (a Python pass over the actual CSV, not eyeballing): 51 such pairs
    exist. 49 of 51 (96%) share **identical** `raw_len`. 46 of 51 (90%)
    show a >30dB RSSI swing between the two, and in every one of those 46,
    one side reads implausibly hot — specifically, 53% of *all 110*
    detections in the run carry RSSI > -25dBm, and that hot value is not
    varied noise: 39 readings sit at **exactly -7.0dBm** and 14 at
    **exactly -6.0dBm** (byte-exact on the SX126x's 0.5dB/LSB grid — bytes
    14 and 12), with only the run's final ~4 minutes drifting to a
    different but still-tight band (-11/-14/-16/-20dBm).
  - **First hypothesis, investigated and mostly ruled out: a firmware bug
    re-logging one physical packet twice.** `radio_task.cpp`'s critical
    section (`getPacketLength()` -> `readData()` -> `getRSSI()` ->
    `getSNR()` -> `startReceive()`) holds the shared-SPI mutex for the
    whole sequence, and `getRSSI()`'s no-arg default (verified against
    RadioLib 7.7.1's actual `SX126x.cpp` source, not assumed) reads packet
    -mode RSSI via `GetPacketStatus`, not the instantaneous/live-channel
    variant — so the obvious "wrong RadioLib call" theory doesn't hold up.
    No smoking gun found in `spi_bus.cpp` either (a plain FreeRTOS mutex
    held for one full transaction, not per-call). Left open, not closed:
    a stale/duplicate FIFO read from a double DIO1 fire was never
    definitively ruled out, just de-prioritized once the alternative below
    turned out to explain the *identical `raw_len`* observation for free.
  - **Second hypothesis, user-supplied and better-fitting: the user's own
    Meshtastic repeater sits within ~5 feet of the receiver during this
    test.** Link budget at 1.5m/915MHz: FSPL ≈ 20log10(1.5) +
    20log10(915) - 27.55 ≈ 35dB. A typical Meshtastic TX power (~20dBm)
    with modest antenna gains (~2dBi each end) puts received power around
    -11dBm before accounting for near-field coupling, orientation, or a
    higher TX power setting — -7dBm at 5 feet is well within physical
    plausibility, not a stretch. This explains every piece of the pattern
    at once, for free: identical `raw_len` (a Meshtastic relay preserves
    payload length — only `hop_limit`/`relay_node` change within the fixed
    header), the 2-9s gap between pairs (Meshtastic's randomized
    rebroadcast delay), the *tight* clustering at a near-constant value
    (a stationary repeater at a fixed distance should read consistently,
    not vary — the tightness that looked like a bug signature is exactly
    what real RF from a fixed-geometry relay would produce), and the
    pattern recurring across 40+ *different* origin node ids (one busy
    local repeater forwards everyone's traffic on a shared regional
    channel, not just one node's). Given this, it's the better-supported
    explanation, though not yet proven — see the fix below for how the
    *next* run settles it outright instead of by inference.
  - **The gap that made this undecidable from the log alone: routing
    metadata was parsed but never logged.** `detection.h`'s Meshtastic
    header parser has extracted `packet_id`/`hop_limit`/`hop_start`/
    `relay_node` since Phase 1 — `test_original_and_relay_share_dedupe_key`
    even exercises real captured original+relay fixtures proving
    `packet_id` matches while `hop_limit`/`relay_node` differ across a
    rebroadcast — but none of the four ever reached `detectionFormatCsv()`
    / `LOG_CSV_HEADER`. So the exact evidence needed to settle "relay" vs.
    "duplicate bug" for certain (matching `packet_id`, decremented
    `hop_limit`, different `relay_node` = relay; identical in all four =
    bug) existed in the firmware's own struct the whole time and simply
    never made it to the SD card.
  - **Fix applied, v0.2.6: wired all four into the CSV**, appended after
    `run` (same append-only-at-the-end convention `rx_uptime_ms`/
    `logger_stack_free`/the GPS diagnostic columns already established).
    Empty (not `00000000`) when no header was parsed, matching
    `channel_or_node_id`'s existing convention; `hop_limit`/`hop_start`
    stay numeric regardless since 0 is a legitimate value there, not an
    absence marker. `DESIGN.md` §8.1 updated to match. New test
    `test_csv_exposes_relay_vs_original` (`test/test_detection/`) asserts
    the CSV row itself — not just the in-memory struct — now shows the
    same `packet_id` with a decremented `hop_limit` and a different
    `relay_node` across the existing original/relay fixture pair. **No
    `pio` in this environment**, so verified the same way the sync-word fix
    was: fetched upstream Unity (ThrowTheSwitch/Unity, `master`) and
    compiled/ran all six native test binaries directly with host g++, not
    just inspection — all **55 tests pass** (12 in `test_detection`, up
    from 11; the other five suites unaffected and unchanged). Still needs
    an actual `pio test -e native` run and a reflash to confirm on the real
    toolchain/hardware. Not a behavior fix — like the GPS ring-buffer bump above, this is
    "make the next run answer the question this one couldn't," which is
    genuinely the best available move here: the alternative was guessing.

- **2026-08-23 (later same day)** — v0.2.6 shipped fast: PR #9 merged CI
  (real `pio run -e cardputer-adv` + `pio test -e native`, not just the g++
  workaround above) before the next bench session, so run0011 (a short,
  ~9-second live capture, not another multi-hour run) came back already
  carrying the new columns. **Settles the relay-vs-bug question raised by
  run0007, definitively, in favor of relay:** three packets, each heard
  twice — `packet_id` `10afda4e`/`384dfe3f` (both from `!bfbc49a2`) and
  `55f3278a` (from `!3b9292f1`) — and in every one of the three pairs,
  `packet_id` matches exactly while `hop_limit` decrements 7->6 and
  `relay_node` changes (`a2`->`5c`, `f1`->`5c`). That is precisely the
  "genuine relay" signature from the DESIGN.md §8.1 fix, and precisely NOT
  what a duplicate-log bug would produce (which would show identical
  `hop_limit`/`relay_node` too). The radio_task.cpp double-DIO1-fire
  question from the run0007 entry is now closed as a non-issue — no further
  action needed there.
  - Worth noting, not investigating further: none of run0011's 6
    detections show the extreme -6/-7dBm pegging that made up 53% of
    run0007 — these read -33 to -56dBm, all physically unremarkable. Not a
    contradiction (this is a 9-second, 3-packet sample, not a comparable
    run), but a reminder that the pegged readings themselves are still
    unexplained in *degree* even though their *mechanism* (relay, not a
    software duplicate) is now settled. `relay_node=5c` matches the last
    byte of `!1bbf065c`, one of run0007's own frequently-heard node ids —
    consistent with one specific nearby node acting as the busy relay in
    both runs, though `relay_node` is only one byte and can't fully rule
    out a different node sharing that byte.
  - **Column order reshuffled, still v0.2.6 -> now v0.2.7**, at the user's
    request after seeing the real output in a spreadsheet: the four new
    routing columns landed append-only at the end (right thing to do while
    they didn't exist yet, awkward to actually read once they did, sitting
    nowhere near the `channel_or_node_id` they describe). New order groups
    columns by what a reader asks first — when/where, then run context,
    then what-kind, then who-and-how-it-got-here (`channel_or_node_id`
    through `relay_node`, now adjacent), then RF params, then signal
    quality, then payload. Full column list and rationale: DESIGN.md §8.1.
    **This is a breaking change to column position**, called out explicitly
    in both `LOG_CSV_HEADER`'s comment and DESIGN.md: any `detections.csv`
    from before this change uses the old order, so position-based parsing
    across the boundary (e.g. concatenating run0007 with run0011+) would
    silently misalign. Every `detections.csv` still carries its own header
    row, so a reader that keys off column *names* rather than position is
    unaffected either way. `test/test_detection/` updated for the new
    layout (field mapping re-verified against the real Unity/g++ workaround
    again, all 12 tests pass) — this was purely a reorder, no column added
    or removed, so `test_header_column_count_matches_row` needed no change.
  - **Raised, not yet acted on: capturing the raw payload bytes for later
    offline decoding.** Currently `buf[256]` in `radio_task.cpp` is read,
    the 16-byte Meshtastic header is parsed out of it, and the rest
    (ciphertext) is discarded the moment the critical section ends — the
    `Detection` struct has nowhere to put it, by design (DESIGN.md §1's
    ~40B queue budget, CLAUDE.md's "no large heap buffers" rule). Genuinely
    useful for later work (MeshOregon-style channels commonly use a
    known/default PSK, so some of this may eventually be decodable
    offline), but it's a real architecture decision, not a small addition:
    growing `Detection` itself blows the documented budget across a
    32-deep queue; a second parallel queue/sidecar file keyed by
    `rx_uptime_ms` avoids that but adds a second SD writer path. Needs a
    decision on capture scope (hex column vs. separate binary sidecar file,
    full payload vs. capped length, whether MeshCore/Reticulum profiles
    even get the same treatment given CLAUDE.md's explicit warning not to
    assume MeshCore's encryption mirrors Meshtastic's) before touching
    code — not started.

- **2026-08-23 (later same day) — Phase 3 built: WiFi AP + web UI
  (`wifi_task`), pulled forward ahead of MeshCore at the user's request.**
  Full plan reviewed and approved before writing code (`EnterPlanMode`),
  grounded in an `Explore` pass over `main.cpp`'s exact boot order/task
  priorities, `config.h`'s existing SD-settings pattern, and
  `logger_task.cpp`'s `SpiBusLock` discipline, rather than guessed. Four
  shape-defining calls were made explicitly with the user (`AskUserQuestion`)
  before design: **WPA2-PSK, not open** (this device is out in the field
  capturing other people's mesh traffic); **on-demand toggle, off by
  default** (WiFi's RAM/CPU/RF-noise cost must never be present during an
  actual drive unless asked for — the exact risk ROADMAP.md's old "lowest
  priority" stance was about); **a live status dashboard, not a literal
  serial-text mirror** (reuses existing counters, far less invasive than
  shadowing the global `Serial` object project-wide); **settings save to
  SD, apply on reboot, no hot-reload** (never touches `radio_task`'s
  real-time critical section from another task).
  - **This reverses a documented decision, on purpose, backed by a real
    number the original decision didn't have.** ROADMAP.md called WiFi
    "lowest priority," gated behind an `ESP.getFreeHeap()` reading "under
    full load" that didn't exist when that was written. It exists now:
    run0007/run0011 (this same session) measured heap_free settling at
    ~304KB with radio+GPS+logger+display all running, flat with no decline
    across 2.5 hours. That's the actual input the gate was waiting on.
  - **New task**, following the exact pattern of the other four
    (`radio_task`/`gps_task`/`logger_task`/`ui_task`): Core 0, priority 1 —
    same tier as `gps_task`/`ui_task` ("least latency-sensitive"), strictly
    below `logger_task` (2) and `radio_task` (3), which must always win.
    Created at boot but does nothing (no `WiFi.mode()`, no AP, no server)
    until toggled — task creation is cheap, starting the AP is the only
    part with a real cost, so that stays deferred to an explicit operator
    ask. Toggling off does a full teardown (`WiFi.mode(WIFI_OFF)`, not just
    "stop accepting connections") so the cost actually goes away.
  - **Toggle mechanism: a long-press of any key (~1.2s), not a specific
    key.** `ui_task.cpp` still has no sourced Cardputer-ADV row/col
    keymap (CLAUDE.md forbids guessing hardware tables), so `anyKeyPressed()`
    — which never decoded *which* key — was replaced with `pollKeyGesture()`,
    turning the same undifferentiated press/release bit (0x80) into TAP
    (the original page-advance behaviour) or HOLD (WiFi toggle) purely from
    timing between a press and its matching release. Needs no keymap
    because there's no "which key" to get wrong. A new WIFI page shows
    AP state/SSID/IP/client count and the gesture hint itself, so the
    instructions are on the device, not just in this doc.
  - **Library choice: built-in `WiFi.h`/`WebServer.h`, zero new `lib_deps`.**
    `platformio.ini` pins no `espressif32` version, which resolves to
    Arduino-ESP32 **core 2.0.17** (forced by the GFX display library pin —
    see that file's own comment). Popular async web server forks
    increasingly target core 3.x only; rather than gamble on core-2.0.x
    compatibility or force a core bump (risking re-breaking the display
    library the way the GFX 1.4.0 pin already had to work around once),
    this uses the synchronous `WebServer` the core ships with. Its
    blocking-per-request nature is a non-issue: this task is deliberately
    the lowest-priority one in the system, so it blocking *itself* while
    serving a request blocks nothing else.
  - **Static assets: one embedded HTML page (`web_assets.h`, `PROGMEM`),
    no LittleFS/SPIFFS, no partition-table change.** The AP has no internet
    access, so nothing external could load anyway — vanilla HTML/CSS/JS,
    client-side tab switching, no framework. Three tabs: Status (polls
    `/api/status` every 2s — the same counters the Serial `[status]` line
    and `ui_task`'s pages already expose, hand-rolled `snprintf` JSON
    rather than pulling in ArduinoJson for a small fixed schema), Downloads
    (`/api/runs` lists run directories; `/api/runs/<n>/{detections,session}.csv`
    streams them), Settings (`/api/config` GET/POST).
  - **CSV streaming is the one place correctness genuinely mattered.**
    Chunked: each 512B chunk opens the file, seeks, reads, and closes
    inside its own short-lived `SpiBusLock`, released *before* the slow
    part (`server.client().write()` over the actual TCP socket) —
    mirroring `logger_task.cpp`'s `appendToFile()` discipline exactly.
    Holding the bus (or an open SD file handle) across a network write
    would stall the radio task for however long the download takes, which
    is the one thing this whole feature must never do. A real bug caught
    during self-review before this was ever run anywhere: `File::read()`
    returns `int` and can be negative on error — the first draft assigned
    that straight into a `size_t`, which would have turned "read failed"
    into "read ~4 billion bytes" and written garbage from an uninitialized
    buffer under a bogus huge length. Fixed to check the signed return
    before ever casting it.
  - **Settings write path reuses `config.cpp`'s existing validators**
    (`freqInRange`/`sfInRange`/`crInRange` — moved out of that file's
    anonymous namespace and exported as `channelFreqInRange`/etc. so there
    is exactly one copy of each bound, not two that could drift) and its
    existing key=value file format (`writeDefaultConfig()`, reused as-is).
    New `writeChannelConfigToSD()` deletes-then-recreates the file rather
    than truncating in place — a shorter new config must not leave trailing
    bytes of a longer old one behind — and, unlike the boot-time loader,
    acquires `spi_bus.h`'s mutex itself (bounded 2s wait, not
    `portMAX_DELAY`: a settings save isn't worth stalling indefinitely for,
    the web client just gets told to retry).
  - **New `radioActiveChannel()` getter** (`radio_task.h`) exposes the
    channel `radio_task.cpp` actually started with (its internal copy,
    which `main.cpp`'s own global can't see), for the settings page to show
    real current values rather than just re-echoing whatever `config.txt`
    last said — those can differ if the SD card was missing/bad at boot.
  - **Docs**: this is a genuine phase reprioritization, not a footnote —
    ROADMAP.md gets a real Phase 3 entry (renumbering old 3–6 to 4–7),
    the versioning table shifts accordingly, DESIGN.md §2/§9 updated to
    match, and CLAUDE.md's proposed-layout table gets both this phase's new
    files and a correction that was already stale before today: `ui_task.cpp`
    was still marked `[ ]` there despite having shipped in Phase 2 (recorded
    in this very log), and `fingerprint.h`'s phase reference needed bumping
    to match the renumbering.
  - **Verification: strong on host logic, honestly unverified on the two
    things only real hardware can answer.** No `pio` in this environment —
    same g++/Unity workaround as every other change this session; all 55
    existing native tests still pass (nothing new added — `wifi_task.cpp`,
    like `radio_task.cpp`/`ui_task.cpp`/`gps_task.cpp`/`logger_task.cpp`
    before it, is Arduino/hardware-coupled, not host-testable pure logic).
    Every other Arduino-dependent file was read in full and reviewed by
    inspection, not compiled — the same bar this project has applied to
    every hardware-coupled change when no toolchain was available. Two
    things genuinely need real hardware, not just a careful read: (1) the
    heap/counter spike this phase was gated behind — flash it, watch the
    *already-existing* `heap=`/`heapmin=` Serial status line before and
    after a long-press toggle (no separate throwaway spike build needed,
    since the toggle is wired straight into telemetry that already prints
    every 5s), and confirm `radioCrcErrorCount()`/`radioQueueDropCount()`/
    `radioBusMissCount()` stay at 0 with the AP active; (2) an actual
    browser connecting to the AP and exercising all three tabs, including a
    CSV download diffed against the same file read off the card directly.
    Neither has happened yet.
  - **Explicitly deferred, not forgotten**: true serial-text mirroring,
    live settings hot-reload, LittleFS for a real static-asset filesystem,
    `session.csv` WiFi fields (client count/AP uptime), and an
    SD-configurable AP password (currently a fixed default,
    `"loratrace123"` — flagged in `wifi_task.cpp` as a placeholder, same
    "smallest thing that's actually useful" call the rest of Phase 2
    followed). None were in scope for what was asked.

- **2026-08-23 (later same day) — Phase 3 go/no-go: closed, on real
  hardware, first try.** User flashed v0.3.0 and connected a browser to the
  AP. Real numbers, not estimates:
  - **Heap cost of the AP itself: ~55KB.** `heap=268824` in the last
    `[status]` line before `WiFi.mode(WIFI_AP)`/`WiFi.softAP()` ran,
    `heap=212856` in the first stable reading after — matches this same
    log's own earlier estimate ("on the order of tens of KB") almost
    exactly, not just in the right ballpark. Free heap settled around
    206-210KB with the AP active and being used (one browser tab open,
    polling `/api/status`), `heapmin` around 192-197KB after a few
    requests — comfortably above zero, real headroom left over.
  - **The actual invariant held**: `crc_err`/`queue_drop`/`bus_miss`/
    `row_drop` all stayed at 0 for the entire session, AP active the whole
    time, exactly the thing this feature was built specifically not to
    risk. **Phase 3's go/no-go is answered: go.**
  - **Two `WebServer.cpp` log lines showed up** (`_handleRequest():
    request handler not found` and `send(): content length is zero`) —
    investigated against the actual Arduino-ESP32 2.0.17 source
    (`libraries/WebServer/src/WebServer.cpp`) rather than guessed, per this
    project's own standing discipline. Both are benign: the first is
    `WebServer::_handleRequest()`'s own `log_e()` call, which fires
    unconditionally whenever a request doesn't match an exact `server.on()`
    route — true for `wifi_task.cpp`'s own CSV downloads by design (they're
    deliberately routed through `onNotFound()`, not registered directly),
    not a sign the handler failed to run. The second fires whenever `send()`
    is called with an empty content-string argument — exactly what
    `streamCsvFile()` intentionally does (headers first, real bytes
    streamed manually afterward) — and the source confirms the response
    still uses the real `setContentLength()` value in its headers
    regardless, so the warning doesn't indicate a truncated or broken
    download.
  - **Added: connect/disconnect logging.** `WiFi.softAPgetStationNum()`
    was already live in `/api/status` and the WIFI page, but only as
    something you'd notice by comparing two readings — nothing announced
    the moment itself. `wifiTask()`'s loop (already running every ~2ms
    while the AP is active) now edge-detects the station count against its
    last-seen value and prints `[wifi] client connected, N total` /
    `[wifi] client disconnected, N total` the moment it changes. Polled in
    the task's own loop rather than via `WiFi.onEvent()` on purpose — that
    callback runs outside this task's context, and every other piece of
    wifi_task state deliberately stays inside its own loop; one less
    cross-context question for a feature this small. Not yet re-verified
    on hardware — build-clean by inspection like every other change made
    this way in this log.
  - **Two hardware-reported gaps fixed the same session:** a config save
    from the web Settings tab left no trace in the serial log (added —
    `writeChannelConfigToSD()` now prints the values written and success/
    failure, mirroring `loadChannelConfigFromSD`'s existing prints), and
    one boot's `[wifi] AP started` line printed with the SSID missing
    between the colon and the IP, though several other boots in the same
    session showed it correctly. No definitive root cause found by
    inspection (`ESP.getEfuseMac()` is a deterministic hardware read, and
    nothing in `startAp()` should be able to touch a local buffer between
    filling it and printing it) — hardened anyway by computing the SSID
    once into a static buffer (`ssidCached()`) instead of a fresh stack
    buffer per AP start, removing a category of doubt even without a
    confirmed cause. Worth watching for a recurrence.

- **2026-08-23 (later same day) — Profile switching: confirmed as design
  (DESIGN.md §5 already says "operator-selected via keyboard... mutually
  exclusive"), deliberately deferred to Phase 4.** User asked about adding
  a keyboard gate to toggle Meshtastic scanning on/off, reasoning ahead to
  needing a Meshtastic/MeshCore switch once Phase 4 lands (not both at
  once). Confirmed this isn't a new architectural decision — DESIGN.md §5
  already committed to exactly this. But `radio_task.cpp` is currently
  locked to a single `ChannelParams` for the whole run with no runtime
  switch logic at all, and MeshCore itself isn't built yet — building the
  actual switcher now would mean designing and testing it against a
  profile that doesn't exist. Asked the user directly (three options: wait
  for Phase 4, build a smaller pause/resume primitive now, or build the
  full switcher scaffolding speculatively); **chose to wait for Phase 4**.
  ROADMAP.md's Phase 4 entry updated to say so explicitly, so this doesn't
  need re-deciding when that phase starts.

- **2026-08-23 (later same day) — Root-caused the missing-SSID/garbled-log
  pattern: unsynchronized Serial access across tasks, likely across cores.**
  run0004's hardware log surfaced a much clearer example than the earlier
  SSID incident: `[config] Wrote /loratrace/config.txt: 8 BW5 sync — reboot
  to apply.` — should have read `918.500MHz SF8 BW125.0 CR4/5 sync 0x2B`.
  Comparing the two side by side makes the mechanism obvious: several whole
  pieces of a 12-call `Serial.print()` sequence went missing (the
  `918.500`/`MHz SF` piece, `125.0`/` CR4/` piece, and `0x2B`), while others
  survived intact — exactly the signature of another task's own Serial
  output landing in the *gaps between* this sequence's individual calls,
  not corruption within any one call. The earlier SSID-caching fix
  addressed a real risk (recomputing into a fresh stack buffer per AP
  start) but not the actual mechanism, which this second, cleaner example
  makes unambiguous. Leading theory, not yet proven on a scope/logic
  analyzer but consistent with everything observed: `main.cpp`'s `loop()`
  (Arduino's own task, very likely Core 1 by default — the same core as
  `radio_task`) and `wifi_task` (explicitly Core 0) both call `Serial.print()`
  with nothing serializing access across that core boundary. This wasn't
  visible before Phase 3 because nothing printed multi-part Serial messages
  often enough to collide with `loop()`'s own frequent multi-part `[status]`
  line — `wifi_task`'s new prints (AP start, client connect/disconnect,
  config-save confirmation) are exactly the kind of infrequent-but-real
  contention that finally made it visible.
  - **Fix: build one buffer via `snprintf`, then a single `Serial.println()`
    call — everywhere a message was being assembled from multiple separate
    `Serial.print()` calls.** A single `write()`-style call to the Serial
    driver is far more likely to be atomic (the driver's own internal
    buffer/FIFO handling typically holds its own critical section for one
    call) than N separate calls with nothing stopping another task's call
    from being fully inserted between them. Applied to: `main.cpp`'s
    periodic `[status]` line (the highest-value fix — this is the project's
    primary diagnostic output, and it was equally exposed even though it
    hadn't shown *visible* corruption yet), `wifi_task.cpp`'s AP-started
    line and client connect/disconnect line, and `config.cpp`'s
    `writeChannelConfigToSD()` success/failure messages (the ones actually
    caught garbled). Every new buffer's worst-case length was measured
    (not guessed) before sizing it — one, `config.cpp`'s, was caught
    genuinely undersized (96 bytes budgeted, 97 needed for the longest real
    message) during this same pass and fixed to 128 before it shipped.
  - **Not yet re-verified on hardware** — same caveat as everything else
    built without `pio` in this session. The theory explains every piece of
    evidence seen so far and the fix is safe regardless of whether the
    cross-core detail is exactly right (a single-call message is strictly
    safer than a multi-call one no matter which two tasks are actually
    racing), but confirming the garbled-log symptom is actually gone still
    needs a real run with WiFi active and multiple config saves/AP
    restarts. Watch for a recurrence — if the *same* pattern (a coherent
    message with whole known pieces missing) still shows up post-fix, the
    theory needs revisiting rather than assuming a smaller residual case
    was missed.

- **2026-08-23 (later same day) — run0010: first combined-load run (WiFi
  AP + GPS + real Meshtastic traffic together, ~29 min end to end, v0.3.0).**
  Closes Next-steps item 1a and gives Phase 3's go/no-go a second, harder
  data point than the original isolated test. Reported in two parts while
  the run was still going; the numbers below are from the full run.
  - **Item 1a closed: genuine relay traffic confirmed, not duplicate
    detection.** Three separate `packet_id`s each seen twice within
    seconds, in every case with `hop_limit` decremented by exactly 1 and a
    different `relay_node` on the second sighting — precisely the signature
    Next-steps item 1a said would settle this: `1335d28a` (hop 7→6, relay
    `68`→`5c`), `66e2811f` (hop 4→3, relay `5c`→`68`), `069a4065` (hop 2→1,
    relay `5c`→`68`). `hop_start` stayed `7` across all of them
    (self-consistent, not corrupted). The duplicate-detection/double-DIO1-fire
    theory this was meant to rule out is closed.
  - **RSSI clustering by `relay_node` holds up over the full run — 45 of 46
    detections, not just the first handful.** `relay_node` `5c`: 23
    sightings, every one -6 to -10dBm (~12-13.5dB SNR). `relay_node` `68`:
    22 sightings, every one -60 to -66dBm. Two disjoint, non-overlapping
    bands across 21 distinct origin nodes over 29 minutes — far too
    consistent to be coincidence, and confirms this reads as two specific
    physical relays (one near-field, one farther) rather than run-to-run
    noise. Checked `radio_task.cpp` for the obvious cause first:
    `getRSSI()`/`getSNR()` are already read before `startReceive()`
    re-arms (comment at the call site notes exactly why — `GetPacketStatus`
    reports the *last* packet, so reading after re-arm would return a stale
    value), so this isn't the classic ordering bug.
  - **One exception is itself informative: `dc259b8e` (`!3b9292f1`) was
    seen twice at the *same* `hop_limit` (6), not decremented** — relay
    `68`→`5c`, RSSI `-62`→`-7`, 8s apart. Every other one of the 20
    multi-sighting `packet_id`s this run (19/20) shows the clean
    decrement-and-relay-change signature from Item 1a above. This one
    reads as two different physical nodes independently forwarding the
    same origin transmission at the same hop distance — not a logging
    artifact, because a duplicate-detection bug wouldn't be expected to
    also flip RSSI by 55dB in lockstep with `relay_node`. Consistent with,
    not contradicting, the relay conclusion.
  - **Health, combined load:** `crc_err`/`queue_drop`/`bus_miss`/
    `row_drop`/`bus_contention` all 0 for the entire ~29 minutes (boot to
    last row), AP active and serving at least one client (the operator's
    dashboard screenshot shows `WIFI CLIENTS: 1`) the whole time, 46 real
    Meshtastic detections across 21 distinct origin nodes logged alongside
    it — including a burst of ~11 detections inside one 60s window
    (23:44:43-23:45:43) that stayed clean. First time all three load
    sources (WiFi, GPS, active mesh RX) have been confirmed together, and
    now under a real traffic burst too — the original go/no-go only had
    WiFi plus idle `/api/status` polling.
  - **`heap_min` step pattern matches the no-leak signature Next-steps item
    2 describes, plus one confirmed-benign step down.** `268508`(boot)→
    `212276`(AP up, ~64s, matches the ~55KB AP cost already on
    record)→`186348`(184s)→flat at `177344` for ~10 minutes (244s–785s)→
    `128112` at 845s, **then flat at `128112` for the rest of the run**
    (845s–1748s, ~15 more minutes, no further decline). `heap_free` stayed
    in its normal ~204-210K jitter range throughout, including across the
    128112 step, so this closes as one transient large allocation (likely
    a client CSV download — the screenshot's `Downloads` tab is the
    obvious candidate) rather than a leak: a leak would show `heap_free`
    trending down too, and it didn't, for the full remainder of the run.
  - **`max_flush_ms` watermark moved twice: 29ms→30ms→39ms**, the last
    jump landing exactly inside the 23:45 detection burst noted above
    (`rows` jumped from 22→33 in that one 60s window, `flushes` only
    28→lagging `rows` by several, i.e. the batched-flush path visibly
    kicking in under load, as designed). 39ms is the new number to beat
    per Next-steps item 2's framing — still well short of anything that
    would starve the radio task, but worth watching if a future burst
    pushes it further.
  - **GPS held up under the combined load, including through the burst**:
    TTFF 158s, sats used up to 21, `nmea_bad_crc` 0/32604 (0.00%,
    consistent with run0007's closed fix), `gps_max_loop_gap_ms` flat at
    352ms for the *entire* run — unmoved even during the 23:45 burst that
    pushed `max_flush_ms` up. No sign `wifi_task` or the heavier logger
    load starves the GPS parse loop.

- **2026-08-24 — Phase 4 (MeshCore profile) built.** DESIGN.md §5 already
  decided the shape ("operator-selected... mutually exclusive"); this session
  built the mechanism, deliberately deferred from Phase 3 (2026-08-23 entry
  above) so it could be built against a real second table instead of a stub.
  - **Live-switch mechanism, `radio_task.cpp`:** a depth-1 `xQueueOverwrite`
    mailbox (`PendingSwitch{profile, channel}`) plus `xTaskNotifyGive` to
    wake the radio task promptly even if it's parked in the existing 5s
    liveness wait. The radio task checks the mailbox first on every wake,
    before treating the wake as a DIO1 packet IRQ — so a switch request and a
    genuine packet arriving in the same instant can't be confused for each
    other, and a switch never has to wait on the 5s timeout to be noticed.
    Chose this over reusing the DIO1 notification's own count directly
    (ambiguous: a coalesced notify count can't say *which* of "packet" or
    "switch" happened, or both) and over a second, separately-blocking queue
    receive (would violate "the radio task never blocks" for an idle-wait
    that has nothing to do with SD or another task). Cost of a switch racing
    a genuine in-flight packet: the packet is lost (radio.begin() for the new
    profile reconfigures the modem) — accepted, since a requested switch
    means the operator no longer wants the old profile's traffic.
  - **Gesture, `ui_task.cpp`:** extended the existing tap/hold duration
    bucketing (already used for page-advance vs. the Phase 3 WiFi toggle)
    with a third bucket at 3000ms, comfortably past the existing 1200ms WiFi
    threshold so the two holds don't feel like the same gesture at different
    speeds. Considered a double-tap instead (would avoid adding a third
    duration tier) but rejected it: distinguishing double-tap from two
    single taps requires delaying every ordinary tap's page-advance by the
    double-tap window while waiting to see if a second tap follows, which
    would make the existing, frequently-used tap-to-advance gesture feel
    laggy for the sake of a gesture that's used rarely. A third hold bucket
    costs nothing on the common path.
  - **MeshCore header parsing scope: deliberately not attempted.**
    `detectionApplyMeshtasticHeader()` is Meshtastic's verified 16-byte
    to/from/id/flags layout (detection.h) — running it against MeshCore
    bytes would produce node_id/packet_id/hop values that *look* valid but
    describe nothing real, since MeshCore's own header format isn't
    reverse-engineered and DESIGN.md §7/CLAUDE.md's house rule both say not
    to assume it mirrors Meshtastic's. `radio_task.cpp` gates the call on
    `activeProfile == MissionProfile::MESHTASTIC`; a MeshCore detection logs
    RSSI/SF/BW/timing and the profile tag (exactly ROADMAP.md Phase 4's
    "basic detection... not payload decode" scope) with those columns empty,
    same convention `detection.h` already uses for a runt Meshtastic frame.
  - **Boot profile unchanged.** `main.cpp` still always starts on Meshtastic
    (plus its existing SD-config override, unchanged) — MeshCore is reachable
    only via the runtime gesture. Considered adding a `profile=` key to
    `config.txt` so an operator could boot straight into MeshCore, but that's
    scope ROADMAP.md's Phase 4 entry doesn't ask for and would need its own
    validation/fallback design; left for a future ask rather than built
    speculatively.
  - **Verification: strong on host logic, honestly unverified on hardware.**
    No `pio` in this environment — same g++/Unity workaround as every prior
    session (a local, throwaway Unity-macro shim, not committed). All 55
    prior native tests still pass, plus 4 new ones (`channelParamsForProfile`
    resolving both tables correctly and falling back to Meshtastic for
    Reticulum/General Exploration, `nextHomeListenProfile` actually
    toggling and returning to start, and a MeshCore CSV row confirming the
    classification column reads "meshcore" with the id columns empty) — 59
    total. `radio_task.cpp`/`ui_task.cpp`/`main.cpp` are Arduino/FreeRTOS/
    RadioLib-coupled like every other hardware-facing file in this project
    and were read in full and reviewed by inspection rather than compiled,
    same bar this project has applied throughout when no toolchain is
    available. See the Phase 4 checklist entries above and Next-steps item 6
    for exactly what a hardware session still needs to confirm.
  - **Also corrected while touching these files:** several comments across
    `channel_plans.h`, `detection.h`, `radio_task.h`, and this doc's own
    Build-order checklist and Next-steps list referenced stale phase numbers
    (a leftover from WiFi being pulled forward ahead of MeshCore, 2026-08-23)
    — e.g. `detection.h` called fingerprinting "phase 4" when ROADMAP.md's
    actual numbering now puts it at phase 5. Fixed opportunistically rather
    than left to compound; ROADMAP.md's numbering is the one source of truth
    for phase numbers, per this doc's own stated relationship to it.

- **2026-08-24 — Phase 5 (on-device menu UI) built, pulled forward ahead of
  `DISCOVERY_SWEEP`/`ENERGY_SWEEP`.** User asked directly for a real menu —
  settings, mode toggles, screens showing profile-relevant info — to build
  the rest of the project around, and to bump it to Phase 5 (same
  restructuring precedent as WiFi's Phase-3 pull-forward). Since Phase 2,
  `ui_task` had avoided decoding individual keys at all, citing "no sourced
  Cardputer-ADV row/col-to-character map" — this session closed that gap.
  - **Correction surfaced during planning:** the user's framing assumed
    dedicated arrow keys exist. They don't. Checked against three
    independent sources before designing anything against it: M5Stack's own
    official Cardputer keyboard API docs, RetroBreeze's
    `cardputer-keyboard-reference` (explicitly covers the Cardputer-**ADV**
    TCA8418 variant, not just the base Cardputer), and `bmorcelli/Launcher`'s
    own shipped, running Cardputer-ADV interface code (already this
    project's established source for the TCA8418 wake sequence, GPIO5/NSS
    timing, and TFT offsets). All three agree: directional intent is Fn held
    + `;`/`,`/`.`/`/` for up/left/down/right — no physical arrow cluster.
  - **Asked the user three questions before designing further** (navigation
    scheme given no arrow keys, whether to also support on-device numeric
    settings editing, whether to keep the old hold-gestures as shortcuts).
    Chosen: plain `,`/`.` to move (no Fn chord) with Enter to select and
    Backspace to go back; toggles only, no numeric entry (channel-param
    editing stays on the Phase 3 web UI/`config.txt`); menu-only, the two
    old hold-gestures removed rather than kept as parallel shortcuts. This
    combination is what turned "decode 56 keys" into "identify 4 keys" —
    the actual scope built.
  - **Sourcing the four keys, `src/keyboard.h`:** chained three citations —
    `Adafruit_TCA8418::getEvent()`'s own doc comment (raw event byte: press
    = key number `K` 1-80 directly, release = `K+0x80`); `bmorcelli/Launcher`'s
    verbatim `mapRawKeyToPhysical()` (raw `K` -> physical row/col in the
    4×14 layout); RetroBreeze's `_key_value_map[4][14]` (physical row/col ->
    which key). Cross-checked the middle+last link against Launcher's own
    input handler, which recognizes Enter and Backspace by `col == 13` —
    matches RetroBreeze's table independently. Inverted the formula for
    just Backspace/Enter/Comma/Period to get four raw press-byte constants
    (65/67/54/58) — full derivation kept in `keyboard.h`'s own comments and
    DESIGN.md §10 so a future reader doesn't have to redo it. **Not
    bench-verified** — see this section's new Phase 5 checklist items.
  - **`radio_task.h`/`channel_plans.h`/`wifi_task.h`: no changes.** Every
    action the menu triggers (`radioRequestProfileSwitch`,
    `nextHomeListenProfile`, `radioActiveProfile`, `radioActiveChannel`,
    `wifiToggle`, `wifiIsEnabled`) already existed from Phase 3/4 — this
    phase is UI/input-layer work reusing an already-built, already-tested
    action layer, not new radio or WiFi logic.
  - **New CHANNEL status page** (`ui_task.cpp`): read-only
    freq/SF/BW/CR/sync word from `radioActiveChannel()`, closing a real gap
    — previously the actual active RF params were visible only over Serial
    or the web UI's `/api/config`, with no on-device way to confirm a
    menu-triggered profile switch actually retuned the radio.
  - **Verification: strong on host logic, honestly unverified on hardware.**
    No `pio` in this environment — same g++/Unity workaround as every prior
    session. Added `test/test_keyboard/` (7 tests: the four sourced press
    bytes decode correctly, their release-byte counterparts and a sample of
    unrelated keys' raw bytes all decode to `NONE`) — 66 native tests total,
    up from 59. `ui_task.cpp`/`ui_task.h`/`keyboard.h` are Arduino/FreeRTOS-
    coupled like every other hardware-facing file in this project;
    `keyboard.h`'s own decode logic is pure and directly tested, but the
    TCA8418/display glue in `ui_task.cpp` was read in full and reviewed by
    inspection rather than compiled, same bar this project has applied
    throughout when no toolchain is available.
  - **Also corrected while touching these files:** several stale phase-
    number references in DESIGN.md (§4's CAD-scanning note, the old §9 build
    order) and ROADMAP.md (the WiFi-size-lever note under Distribution still
    said "gated behind Phase 6," and the versioning table's `v1.0` row still
    described the pre-renumbering Phase 7) — left over from earlier
    renumbering passes that didn't reach every mention. Fixed opportunistically
    rather than left to compound, same reasoning as the Phase 4 entry above.
- **2026-08-24** — Added digit-key page jumps ('1'-'5', direct to a numbered
  carousel page) ahead of the Phase 5 hardware session, at the user's
  request once they confirmed a full keyboard would be on hand for that
  bench session anyway. Still Phase 5 scope, added before any of it has
  touched real hardware — no version bump (same precedent as the MeshCore
  table existing in `channel_plans.h` before Phase 4 wired it in).
  - **Sourcing, same chain as the original four keys, re-verified rather
    than recalled:** cloned `RetroBreeze/cardputer-keyboard-reference` and
    `bmorcelli/Launcher` fresh this session (read-only) instead of trusting
    memory of their content, given this project's own house rule against
    guessing hardware tables and its history of a silent, costly wrong
    value (the sync-word bug, 2026-08-23). Confirmed `_key_value_map`'s row
    0 (`` ` ``,`1`-`9`,`0`,`-`,`=`,Backspace at columns 0-13 — Backspace at
    col 13 matches the already-bench-pending constant, a consistency check
    for free) and re-ran `mapRawKeyToPhysical()`'s formula by hand for
    row 0/col 1-5, cross-checking the inversion against the four already-
    derived constants (Comma/Period/Enter/Backspace) before trusting it for
    new ones. Result: K=5/11/15/21/25 for '1'-'5'
    (`KEY_RAW_1_PRESS`..`KEY_RAW_5_PRESS`, `src/keyboard.h`).
  - **Weaker sourcing than Backspace/Enter, flagged rather than glossed
    over:** those two had a second independent confirmation (Launcher's
    input handler recognizing both by `col == 13`); the digit keys only have
    RetroBreeze's table plus the formula, the same bar Comma/Period already
    cleared. Reflected in a strengthened PROGRESS.md checklist item asking
    for all five to be pressed individually, not just a couple as a spot
    check.
  - **Design choice: plain `KeyAction::JUMP_1..JUMP_5`, not a `JUMP` action
    carrying a page index.** Keeps `keyboard.h` free of any dependency on
    `ui_task.h`'s `UiPage` enum — `ui_task.cpp` does the 1:1 index mapping
    itself (`jumpToPage()`). Digit keys are carousel-only; the menu ignores
    them rather than repurposing them for its own two-item selection.
  - **Verification: host-native only, same bar as the rest of Phase 5.**
    `test/test_keyboard/` grew to cover all five new press bytes, their
    release-byte counterparts, and the immediately-adjacent unmapped digits
    ('6'-'9'/'0') as an allowlist boundary check — the case most likely to
    catch an off-by-one in the new derivation. Not yet bench-verified
    against a real TCA8418, same as the original four.
- **2026-08-24 (later same day)** — First real Phase 5 hardware pass, and
  two live findings from it acted on immediately. User confirmed on real
  hardware: all five digit-jump keys work, Comma/Period move the
  carousel/menu as designed. That same session surfaced two things only a
  human hand on real keys was going to find:
  1. **Backspace felt wrong for "leave the menu."** Swapped for the
     top-left key instead — silkscreened ESC on the physical Cardputer-ADV
     keycap, even though the TCA8418/RetroBreeze's own reference call its
     base character backtick. Bound as a **plain** press (no Fn), per this
     project's existing "no Fn chord" rule — RetroBreeze documents the
     upstream convention as Fn+backtick ("There is no dedicated ESC key in
     the firmware... ESC is accessed as Fn + backtick"), but this firmware
     doesn't track Fn as a modifier at all and isn't starting now for one
     key. `KEY_RAW_BACKSPACE_PRESS` removed outright rather than left
     dead — nothing else used it. New constant `KEY_RAW_ESC_PRESS = 1`
     (physical row 0, col 0).
  2. **The operator, working from the same keycaps, tried the printed
     Fn-arrow diamond** (`;`/`,`/`.`/`/` = up/left/down/right — also
     RetroBreeze-documented) expecting it to double as navigation.
     Reported back precisely: Left (`,`) and Down (`.`) already worked —
     unsurprising, they're literally Comma/Period, already bound — while Up
     (`;`) and Right (`/`) silently did nothing, because nothing in
     `keyboard.h` had ever mapped them. Not a bug (the allowlist's whole
     design point is that an unmapped key does nothing) but a real UX gap
     once a full keyboard was actually in hand to notice it. Fixed by
     adding `KEY_RAW_SEMICOLON_PRESS`/`KEY_RAW_SLASH_PRESS` as plain-press
     **aliases** for the existing `KeyAction::PREV`/`NEXT` — no new
     `KeyAction` values, since semantically these are exactly the same
     "previous"/"next" the carousel and menu already had, just reachable
     from a second physical key each. All four arrow-diamond keys (and
     digits `1`-`5`) work without holding Fn.
  - **Sourcing:** same three-source chain as everything else in
    `keyboard.h`, re-verified against the already-cloned RetroBreeze/
    Launcher checkouts from the digit-key entry above rather than
    re-cloned. RetroBreeze's README documents both quirks explicitly (its
    own ESC and Fn-arrow-diamond sections, cited in `keyboard.h`'s
    comments and DESIGN.md §10) — this wasn't inferred from the key-value
    table alone, it's stated outright as the intended role of these keys'
    printed keycaps.
  - **`KEY_RAW_ESC_PRESS`/`KEY_RAW_SEMICOLON_PRESS`/`KEY_RAW_SLASH_PRESS`
    not yet bench-verified** — added and tested host-side same day as the
    report that motivated them, not yet re-flashed and pressed. Tracked as
    its own checklist item above rather than folded into "closed," since
    conflating an untested change with an already-confirmed one is exactly
    the kind of thing this log exists to avoid.
  - **Test suite:** `test/test_keyboard/` renamed/added cases for the new
    aliases (`test_semicolon_is_prev`, `test_slash_is_next`,
    `test_esc_is_back`) and added `test_backspace_is_no_longer_mapped` as
    an explicit regression pin — Backspace returning to "just an ordinary
    ignored key" is an intentional behavior change, not an oversight, and
    deserves its own test rather than quietly losing coverage when its old
    test (`test_backspace_is_back`) was removed. 70 native tests total, up
    from 67 (3 new: two alias tests plus the regression pin; the digit-jump
    test from the prior entry already counted). Verified with the same
    g++/Unity workaround (no `pio` in this environment) — all 70 pass.
- **2026-08-24 (later same day)** — Per-profile channel config, requested by
  the user after noticing the web settings page (screenshot: the old
  single-panel "Active LoRa channel" form) only ever had one slot while
  Meshtastic and MeshCore run genuinely different frequencies. Investigating
  turned up a real bug, not just a missing nice-to-have:
  - `main.cpp` always boots `MissionProfile::MESHTASTIC`, and the old
    `loadChannelConfigFromSD()`/`writeChannelConfigToSD()` only ever
    read/wrote ONE shared `ChannelParams` applied to that boot profile.
    `radio_task.cpp`'s live profile switch (`radioRequestProfileSwitch()`)
    called `channelParamsForProfile()` directly — the hardcoded table,
    never the override — so **switching to MeshCore and back to Meshtastic
    silently reverted Meshtastic to its hardcoded default**, dropping
    whatever the operator had configured (e.g. their real MeshOregon-style
    918.5MHz/SF8/BW125/CR4:5 settings, see the 2026-08-22 entries above).
  - Worse: `handleConfigGet()`/`handleConfigPost()` read/wrote whatever
    `radioActiveChannel()` currently was — **while active on MeshCore, the
    Save button captured MeshCore's values and wrote them into the one
    file that only ever gets read back as a *Meshtastic* override on the
    next boot.** The device would then boot claiming Meshtastic while
    actually tuned to MeshCore's frequency — profile label and radio config
    silently disagreeing. This was reachable in the shipped Phase 3/4 web
    UI, not a hypothetical.
  - **Fix: `ProfileOverrides` (channel_plans.h)** — two independent slots
    (`meshtastic`/`meshcore`, each with its own `_set` flag) instead of one
    shared `ChannelParams`, plus `resolvedChannelForProfile(overrides,
    profile)` — pure, host-testable — that returns the override if set,
    else `channelParamsForProfile()`'s hardcoded table. Both the initial
    boot channel (`main.cpp`) and every later profile switch
    (`radio_task.cpp`) now go through this one function, which is what
    keeps them from disagreeing the way the old design did. `radio_task.cpp`
    holds its own copy (`activeOverrides`, set once at `radioTaskStart()`,
    read-only after) and exposes it via a new `radioActiveOverrides()`
    accessor — same small-POD, no-lock convention as `radioActiveChannel()`.
  - **config.txt schema, breaking change:** `freq_mhz=`/`sf=`/etc. becomes
    `meshtastic_freq_mhz=`/`meshcore_freq_mhz=`/etc. — prefixed per profile.
    `config.cpp`'s `applyConfigLine()` now dispatches on the prefix to a
    `ChannelParams*`/`bool*` pair rather than one shared struct; validation
    (`channelFreqInRange()` etc.) is unchanged and still the single shared
    source of truth wifi_task validates against. Deliberately not kept
    backward-compatible with the old unprefixed keys — this project is
    pre-1.0 and its own fails-safe design already handles this cleanly (an
    old-format key is simply "unrecognized," logged and skipped, falling
    back to the hardcoded default) rather than needing a compatibility
    shim. **Operational consequence flagged in the Phase 3 checklist above:
    this device's own SD card still has the old-format file** and needs
    updating before its next bench session, or its Meshtastic override will
    silently stop applying.
  - **`writeProfileConfigToSD(profile, params, current)`** replaces the old
    `writeChannelConfigToSD(params)`: takes the profile being saved plus
    the full currently-loaded `ProfileOverrides`, overlays just that one
    profile's new values, and rewrites the whole file with BOTH profiles'
    blocks — the other profile's block is carried through unchanged from
    `current`, never clobbered. `writeFullConfig()` (renamed from
    `writeDefaultConfig()`) writes both blocks unconditionally so the file
    is always complete and valid, whether or not either profile actually
    has an override set.
  - **Web UI (`web_assets.h`):** the single "Active LoRa channel" form
    (screenshot) becomes two independent panels, "Meshtastic preset" /
    "MeshCore preset", each with its own five fields and Save button
    (`configForm-meshtastic`/`configForm-meshcore`, field ids prefixed
    `mt_`/`mc_`). A small "active" badge next to whichever profile
    `GET /api/config`'s new `active_profile` field names, so the page makes
    it visually obvious which panel's values the radio is *actually* using
    right now — the exact confusion (label says one thing, radio does
    another) the bug above allowed. `POST /api/config` now requires a
    `profile` field naming which panel's Save fired; without it (or an
    unrecognized value) the request is rejected with 400 rather than
    guessing. One shared JS submit handler bound to both forms
    (`form.dataset.profile`/`form.dataset.prefix`) rather than duplicating
    the handler twice.
  - **Kept intentionally out of scope, confirmed with the user first:** no
    new on-device menu item. The existing Profile-switch menu action
    (`radioRequestProfileSwitch()`/`nextHomeListenProfile()`, Phase 4/5)
    already covers "switch between defaulted frequencies for each
    function" once it resolves overrides correctly, which is exactly what
    this fix does — a second menu item would have been redundant. Also
    unchanged: the existing "not live, reboot to apply" boundary — a web
    Save still doesn't hot-reload the running SX1262, matching the
    boundary the original single-preset design already had (and the same
    reasoning: radio_task.cpp still never touches SD, keeping its
    switch-mailbox path exactly as fast and simple as before).
  - **Verification:** `test/test_channel_plans/` gained three cases for
    `resolvedChannelForProfile()` (no-override matches hardcoded; a set
    override is used and the two profiles stay independent; Reticulum/
    General Exploration fall through to Meshtastic's *resolved* channel,
    override included, mirroring `channelParamsForProfile()`'s existing
    fallback) — pure logic, host-testable, no I/O. 73 native tests total,
    up from 70, g++/Unity workaround, all passing. `config.cpp`/
    `wifi_task.cpp`/`main.cpp`/`radio_task.cpp` themselves are
    Arduino-dependent like the rest of this project's hardware-facing code
    and were reviewed by inspection rather than compiled (no `pio` in this
    environment) — genuinely unverified until the next hardware session,
    tracked in the Phase 3 checklist above rather than claimed done.
  - **Version: v0.5.0 -> v0.5.1.** PATCH-level per CLAUDE.md's own rule
    ("PATCH for fixes adding no phase scope") — this hardens already-shipped
    Phase 3/4 behavior (and fixes the real bug above) rather than landing
    new build-order scope, so MINOR stays at Phase 5's `0.5`.

- **2026-08-24 (later same day) — Phase 5 bench pass finished; every
  checklist item closed, plus a UX rework it surfaced.** Claude Code, live
  in VS Code with the Cardputer-Adv on USB (`/dev/ttyACM1`), ran the
  session: flashed current `main`, then walked the user through the
  remaining Phase 5 checklist interactively, watching serial in the
  background the whole time for any crash/exception/counter regression.
  - **ESC/backtick and Semicolon/Slash: confirmed working exactly as
    designed.** First items in this file to move from "sourced" to
    "bench-confirmed."
  - **UX finding, fixed the same session:** the user found opening the menu
    with Enter "feels kind of weird" and asked whether ESC could open it
    instead, with Enter narrowed to just acting on the highlighted row.
    Confirmed with the user which of two options they wanted (ESC opens it
    *and* Enter still also would, vs. ESC becomes the only way in) before
    touching code — picked the latter. Changed in `ui_task.cpp`'s carousel
    branch only: `KeyAction::SELECT` (Enter) no longer opens the menu
    (now a no-op there, same as it already was in the menu's own PREV/NEXT
    context); `KeyAction::BACK` (ESC) does instead, alongside its existing
    menu-closing job — one key, two directions, dispatched on `UiMode`.
    `keyboard.h`'s raw-byte decode didn't change at all (Enter still
    decodes to `SELECT`, ESC still to `BACK` — the mapping is
    context-free); only `ui_task.cpp`'s interpretation of those actions
    changed, so `test/test_keyboard/` needed no updates and all 73
    host-native tests still pass unmodified. Footer hint text updated to
    match (`Enter menu` -> `` ` menu ``). Re-flashed and the user
    re-confirmed all four legs (ESC opens, Enter fires the highlighted
    action, ESC closes, Enter no-ops in the carousel) on real hardware
    before moving on.
  - **Menu actions confirmed via serial, not just the screen.** Profile
    switch was checked against the CHANNEL page. WiFi toggle was checked
    against the serial log directly: `[wifi] AP started: LoRaTrace-7850 @
    192.168.4.1` appeared right on cue, and — bonus, not something this
    session set out to test — it answered Phase 3's WiFi heap/counter
    go/no-go a second time, under real traffic this time rather than an
    idle AP: heap went 267580 -> 211388 on AP-up (~56KB, matching the
    2026-08-23 measurement in the Phase 3 entry above almost exactly),
    `crc_err`/`queue_drop`/`bus_miss`/`row_drop` stayed at 0 through 800+
    real detections logged with the AP active, and heap settled at ~250K
    (not all the way back to 267580, but flat/non-decreasing over
    repeated readings — read as post-burst allocator fragmentation from
    805 flushed rows, not a leak, but worth another look if a future
    session sees heap trend downward over many AP on/off cycles rather
    than just settling once).
  - **User forgot to toggle WiFi back off** after confirming it worked
    (their own words: "i thought you wanted to test that") — turned off
    on the next round of instructions with no issue. Not a firmware defect,
    just an interactive-session loose end; noted here only because
    CLAUDE.md's house rule is the AP should stay off unless deliberately
    on, and it's worth being deliberate about closing that loop each
    session rather than assuming it happened.
  - **Test suite:** unchanged at 73 host-native tests (see above — the
    ESC-opens-menu change lives entirely in `ui_task.cpp`, which isn't
    host-testable the way `keyboard.h` is).
  - **Version: v0.5.1 -> v0.5.2.** PATCH-level — this closes out
    already-shipped Phase 5 scope (bench verification) and reworks its
    trigger key, not new build-order scope.

- **2026-08-24 (later same day) — Verbose serial debug mode added, then
  used to close out Phase 4's bench verification the same session.** The
  user asked to check RSSI/SNR for the live MeshCore traffic already
  coming in, and there was no cheap way to: neither the serial `[status]`
  line nor the web dashboard surfaces per-packet detail, only
  `detections.csv` does, which meant an SD pull or standing up the WiFi AP
  and joining it from this machine just to check a number. The user
  floated a debug-mode idea unprompted — a menu-toggled verbose serial
  log, possibly also a serial *command* console. Scoped down to
  output-only before writing anything (asked first, given the real
  architecture question a command parser would raise on an RX-only tool);
  input control stays a future ask if it turns out to be needed.
  - **Implementation:** a third menu row, `Debug` (`MENU_ITEM_COUNT` 2 -> 3,
    `ui_task.cpp`), toggling `loggerDebugToggle()` (`logger_task.cpp`, new).
    Deliberately lives in `logger_task.cpp`, not `radio_task.cpp`: printing
    happens on Core 0 after the detection has already crossed the queue,
    so a slow/absent serial console can never back-pressure the Core 1
    radio task the way a print from inside its own loop could (CLAUDE.md's
    "radio task must never block on non-radio I/O"). Reuses the exact CSV
    row `detectionFormatCsv()` already builds for SD rather than a second
    format call, so the serial line can never drift from what actually
    lands on the card, and prints unconditionally of `sdReady` — the whole
    point is seeing live RX without an SD card or the AP in the loop.
  - **Bug caught and fixed the same session, on real hardware, first
    detection heard:** the first version issued three separate `Serial`
    calls per line (`print` + `write` + `println`) and came out visibly
    torn — `main.cpp`'s Core-1 `[status]` line landed mid-sequence,
    producing garbage like `45eshcore` and a row missing its leading `m`.
    This is the exact failure mode `main.cpp`'s own `[status]`-line
    comment already documents (and the 2026-08-23 `[wifi]` SSID bug hit
    for real) — unsynchronized cross-core `Serial` access tears a
    multi-call sequence when another task's print lands in the gaps
    between calls. Fixed by collapsing both the per-detection line and the
    toggle-on announcement to one buffer + one `Serial.write()` each, the
    same pattern `main.cpp` already uses for `[status]`. Re-flashed,
    re-verified: clean, untorn lines from then on.
  - **Used immediately to close Phase 4's last open item.** With debug
    mode on: MeshCore RX confirmed real and plausible (910.525MHz/SF7/
    BW62.5, RSSI -58 to -64dBm, SNR ~12dB, a dozen+ detections, blank
    `node_id`/`packet_id` as designed since MeshCore's header isn't
    parsed). The user then applied the actual local MeshOregon settings
    (918.500MHz/SF8/BW125.0kHz/CR4:5 — the same numbers Phase 1's
    2026-08-23 entry recorded) to the Meshtastic profile via the web
    Settings tab, power-cycled to apply them (a next-boot-only change, same
    as the channel override always has been), then sent live Meshtastic
    traffic before switching to MeshCore. Both sides logged real, correct
    detections in the same run (run0022): Meshtastic showed a genuine node
    (`!bfbc49a2`) with two packet_ids each seen twice, `hop_limit`
    decremented and `relay_node` changed between sightings on both — the
    same real-relay signature run0007 already validated, not a dedup bug —
    RSSI -42 to -49dBm, SNR 12.5-14.75dB. `crc_err`/`queue_drop`/
    `bus_miss`/`row_drop` never moved because of either switch, `rx`
    climbed steadily throughout. Phase 4's mid-run-switch and live-RX
    checklist items both close on this evidence — see the Build-order
    checklist's Phase 4 entry above.
  - **Test suite:** unchanged at 73 host-native tests — the new code lives
    entirely in `logger_task.cpp`/`ui_task.cpp`'s Arduino-dependent paths,
    same reason the ESC-opens-menu rework needed none.
  - **Version: v0.5.2 -> v0.5.3.** PATCH-level — closes out already-shipped
    Phase 4 scope (bench verification) and adds a debugging aid, not new
    build-order scope.

- **2026-08-24 (later same day) — Real Serial mutex added (`serial_lock.h`/
  `.cpp`); the 2026-08-23 "one buffer, one call" fix wasn't actually
  sufficient, and this session proved it on real hardware.** Mopping up
  Phase 3's two remaining loose ends (connect/disconnect logging, the
  Settings-tab round-trip — both genuinely closed by this session, see the
  Next-steps item 0 update above) required getting a real WiFi client to
  join the AP and save Settings, which is exactly the kind of real
  concurrent load the single-buffer fix was never actually tested against.
  It failed immediately: `[wifi] AP started: L` (everything after one
  letter missing) and a `[config]` MeshCore write missing everything before
  `BW62.5` both came out torn in the same short session, even though both
  already used the "one buffer, one `Serial` call" pattern from
  2026-08-23. That fix's own theory — "a single write() call is far more
  likely to be atomic than several" — turned out to be exactly that, a
  likelihood, not a guarantee, and real contention found the gap.
  - **The actual fix:** `serial_lock.h`/`.cpp`, a FreeRTOS mutex mirroring
    `spi_bus.h`'s `SpiBusLock` pattern exactly (real mutex not binary
    semaphore, for priority inheritance; scoped RAII `SerialLock`, always
    checked with `held()`). Initialized in `main.cpp`'s `setup()`
    immediately after `Serial.begin()`, before any task that might print is
    started. Every existing `Serial` print call site in the whole firmware
    now takes it: `main.cpp` (boot banner, all WARN/FATAL lines, the
    `[status]` line), `wifi_task.cpp` (AP start/stop, client connect/
    disconnect), `logger_task.cpp` (both debug-mode messages),
    `config.cpp` (`writeProfileConfigToSD()`'s confirmation line — the
    genuinely concurrent, runtime entry point), and `gps_task.cpp` (the
    clock-set-from-GPS message, also collapsed to one buffer while it was
    being touched, same pattern as everything else). **Deliberately NOT**
    added to `applyConfigLine()`/`loadProfileOverridesFromSD()`/
    `writeFullConfig()` in `config.cpp` — those are only ever called once
    from `main.cpp`'s `setup()` before any task exists, provably
    single-threaded, and main.cpp's own boot-sequence comment already
    documents that invariant; adding the lock there would just be noise on
    ten-plus print call sites that can't race. Never added to
    `radio_task.cpp`: it doesn't print at all, and CLAUDE.md's house rule
    is it must never block on non-radio I/O — keeping that true was a
    design constraint on this fix, not an oversight.
  - **Re-verified on hardware the same session, with the same real-load
    test that broke the old fix**: WiFi AP started, a phone joined,
    Settings saved for both profiles, phone disconnected. Every message —
    `[wifi] AP started`, `[wifi] client connected/disconnected`, and a
    `[config]` MeshCore write (the exact line that was torn minutes
    earlier) — came through completely intact.
  - **One residual anomaly, not explained by this fix and left as a new
    watch item** (see the watch-items list above): a single `[config]`
    Meshtastic write line lost its prefix even though it went through the
    new lock, while everything immediately around it (including another
    `[config]` write) was clean. Not cross-task interleaving — the message
    printed just before it was itself complete, so nothing landed inside
    this call's critical section. Reads as a USB-CDC-level single-write
    truncation under real WiFi/WebServer load, a different and deeper
    question than what this session set out to fix. Documented rather than
    chased further given the hour and the lack of a logic analyzer in this
    session's toolset.
  - **Test suite:** unchanged at 73 host-native tests — every changed file
    is Arduino-dependent (`Serial`, FreeRTOS), not part of the host-native
    build.
  - **Version: v0.5.3 -> v0.5.4.** PATCH-level — a correctness fix to
    already-shipped diagnostic infrastructure, not new build-order scope.

- **2026-08-25 — UI architecture redesign promoted to Phase 6,
  `DISCOVERY_SWEEP`/`ENERGY_SWEEP` pushed to Phase 7/8.** Session started
  as a question about Phase 6 (`DISCOVERY_SWEEP`) planning; turned into a
  scope decision after the user asked directly whether Phase 5 was really
  finished and floated wanting new features toggled through the UI going
  forward.
  - **Phase 5 status check.** Confirmed complete against its own declared
    scope — PROGRESS.md's checklist and CLAUDE.md's Status section both
    already marked every sub-item closed and hardware-bench-verified
    2026-08-24. Not a reopening of unfinished work. But the check surfaced
    the real issue: the menu had already grown past its own documented
    scope once, silently. `ui_task.cpp`'s own comment still read "the menu
    has exactly two items this phase" while a third row (`Debug`,
    `loggerDebugToggle()`) had been added the same bench day with no
    framework change and no comment update — see this file's 2026-08-24
    entries. `DISCOVERY_SWEEP` would add at least a fourth item the same
    way, `ENERGY_SWEEP` a fifth after it.
  - **User's principle, adopted as a house rule (CLAUDE.md):** new
    operator-facing behavior gets an on-device menu toggle, not a silent
    default or a web-UI-only setting. This was already the de facto
    pattern (WiFi off-by-default+toggle, the MeshCore switch, the Debug
    toggle) but had never been written down, which is exactly how the
    Debug row landed without anyone treating it as a UI decision.
  - **Current UI critique (user, confirmed against `ui_task.cpp`):** the
    five carousel pages (RADIO/CHANNEL/GPS/SYSTEM/WIFI) are single-column
    stacked text (`drawRadioPage()` etc.) — real unused width on the
    240x135 panel, values organized one-per-line rather than grouped.
  - **M5PORKCHOP review (github.com/0ct0sec/M5PORKCHOP), read-only, cloned
    locally for reference — not linked as a dependency, not cloned for its
    content.** It's a Cardputer-ADV WiFi/BLE pentesting tool with the same
    240x135/no-PSRAM hardware constraints as this project, so its UI
    engineering choices are genuinely comparable even though its feature
    set (packet capture, deauth, BLE spam) is not. Reviewed:
    `src/ui/menu.h` — a "Sirloin-style grouped modal": `RootItem` entries
    are either `RootType::DIRECT` (opens a mode) or `RootType::GROUP`
    (opens a `MenuItem` sub-list via `GroupId`), same input model
    throughout. `src/ui/display.h` — a `NoticeKind` (REWARD/STATUS/WARNING/
    ERROR) x `NoticeChannel` (AUTO/TOAST/TOP_BAR) abstraction for transient
    messages, decoupled from whatever screen is showing. `src/ui/
    swine_stats.h` — a tabbed stats screen (`StatsTab`: STATS/BOOSTS/
    WIGLE) as one model for grouping related values instead of one
    per line. Also present, and explicitly **not** part of this redesign:
    a full RPG XP/leveling/achievement system (`src/core/xp.h`, 60
    achievement bitflags, class tiers, buffs/debuffs), an animated ASCII
    mascot with a mood/dialogue system (`src/piglet/`), and a voice/tone
    (its own README: "the difference between tool and weapon is the hand
    holding it. wink. wink.") that's the deliberate opposite of BRAND.md's
    "calm, precise, instrument-like... closer to a survey receiver... than
    a red-team utility or hacker toy" direction and its explicit guardrails
    against mascot/gamified/neon aesthetics. Structure reviewed and partly
    adopted; content, tone, and aesthetic were not.
  - **Three decisions, via AskUserQuestion:**
    1. **Grouped menu**, not a longer flat scrolling list — root categories
       (e.g. Profile, Radio Mode, System) open short sub-lists, same four
       keys throughout. Chosen because a flat list is the thing that's
       already outgrown itself once; scrolling a longer flat list doesn't
       fix that, it just delays the next overflow.
    2. **Add a toast/notice layer**, not inline-only feedback. Its actual
       heap cost is unmeasured and must be measured and reported before it
       ships — same discipline as every other RAM-hungry addition in this
       project (WiFi AP's ~55-56KB number is the precedent this follows,
       not an estimate to trust on paper).
    3. **Adopt BRAND.md's on-device labels now** (`Watch`/`Probe`/`Sweep`
       for HOME_LISTEN/DISCOVERY_SWEEP/ENERGY_SWEEP; `Mesh Trace`/`Core
       Trace`/`Open Trace`/`Spectrum Trace` for the four profiles) as the
       UI's actual display strings — a table BRAND.md has carried since it
       was written but the on-device UI never used. Deliberately kept as a
       separate display layer, not a rename of `detection.h`'s
       `missionProfileName()`: that function's output (`meshtastic`,
       `meshcore`) is what's already written into every `detections.csv`
       a real run has produced, and DESIGN.md §8 already has a "don't
       concatenate runs across a format change without checking the
       header" rule for exactly this kind of risk.
  - **Renumbering applied:** Phase 6 is now the UI architecture redesign;
    `DISCOVERY_SWEEP` moves to Phase 7, `ENERGY_SWEEP` to Phase 8. Updated
    ROADMAP.md (Phases section, Versioning table), DESIGN.md (§1 pin
    table, §7's preamble-length note, §8.3's start/stop-gate note, §9
    build order), CLAUDE.md (proposed-layout's `fingerprint.h` line, new
    house rule, new Status entry), this file's Build-order checklist, and
    README.md's now-doubly-stale "three pages... still Phase 6" paragraph
    (also fixed the page count while in there — it's five pages now, not
    three, independent of today's renumbering). Also fixed forward
    references to the old phase numbers left in code comments
    (`radio_task.h`, `detection.h`, `channel_plans.h` x3, `main.cpp`,
    `board_pins.h`) — same "fix stale phase-number references
    opportunistically" precedent already used for `detection.h`'s
    phase-4-vs-5 fingerprinting comment on 2026-08-24. Historical
    Decisions-log entries above (including this file's own 2026-08-22/23
    "Phase 6" mentions, back when that number meant "the eventual full
    interactive UI") were deliberately left as written — they're a record
    of what was true when they were written, not a live cross-reference,
    same convention this log has followed for every earlier renumbering.
  - **Not done in this session:** no code changes to `ui_task.cpp`/`.h`
    itself. This entry is planning/scope capture only — the grouped-menu
    implementation, the toast layer's real heap measurement, the
    redesigned status-page layouts, and the BRAND.md label wiring are all
    still open work under the new Phase 6 checklist entry above.
- **2026-08-25 — Phase 6 fully implemented (v0.6.1) after mockup review;
  Mesh Trace regrouped as a profile family; design artifact preserved as a
  living reference.** Direct continuation of the same day's entry above.
  v0.6.0 (built earlier the same day) was a first, narrower pass — the
  grouped menu and a second status-page column, reasoned through and
  verified against `pio test -e native`/`pio run -e cardputer-adv` but not
  against any visual check, since this project has no display simulator.
  - **Built a pixel-accurate HTML/Canvas mockup** (not committed to the
    repo — an Artifact) simulating the real ST7789V2 output: transcribed
    directly from `ui_task.cpp`'s actual draw calls (cursor math, RGB565
    colors converted to their real 8-bit sRGB equivalents, a bitmap-style
    font chosen to match the on-device look), with a "before/after" compare
    section built from the literal committed v0.6.0 drawing functions kept
    byte-for-byte unedited alongside the proposed redesign, so the compare
    never blurred what was actually shipped against what was still a
    proposal. Verified with a custom Node.js DOM/canvas-stub harness
    (`dom_harness.js`, not committed) before every republish — real click/
    toggle interactions and multiple animation frames, not just a syntax
    check — which caught three real bugs before they ever reached this
    file: a menu row's label/value separator baked into trailing spaces on
    table entries (moved into the shared row-drawing function instead), a
    toast not drawing at all while the menu was open (fixed by drawing it
    unconditionally at the end of the page-dispatch function regardless of
    branch), and a WiFi-client-count buffer sized dangerously close to
    overflowing on `"255"`.
  - **~9 rounds of concrete visual feedback**, each against a screenshot
    (sometimes annotated) of the mockup, each acted on before the next
    round: dropped the persistent footer key-hint line and the idle
    heartbeat blink; added GPS-fix/heap-health status dots to the header
    plus 5px clearance from the battery reading; moved the active profile
    name and page position out of the header into a new footer status
    line (profile left-anchored, position right-anchored); widened and
    repositioned the right-hand column on RADIO/CHANNEL/GPS/SYSTEM so all
    four land at the same x=170 start GPS's sats/qual column already used;
    redesigned CHANNEL (a frequency-position bar against the SX1262's real
    868–923MHz tuned range, DESIGN.md §1, plus a rough time-on-air
    estimate) after the first cut read as "cluttered and top-heavy";
    redesigned GPS's constellation counts as 4 small bars in the right
    column instead of a dim digit-by-digit text line; widened the gap
    between SYSTEM's two right-hand columns and added a heap-fraction bar
    under "k heap" (against the ESP32-S3FN8's real ~512KB no-PSRAM SRAM
    ceiling, same upper-bound-for-context framing as the frequency bar).
  - **The regroup that triggered BRAND.md's revision.** Mid-review the user
    named a modeling problem directly: "Mesh Trace is a mode, the profile
    selector is a sub of mesh trace as we select what we are tracing."
    First implemented in the mockup as a menu-*structure* change only
    (Profile becomes a GROUP row opening onto Meshtastic/MeshCore), while
    flagging that "Mesh Trace"/"Core Trace" were still kept as two parallel
    BRAND.md names rather than one actually becoming the parent of the
    other — and asking whether the user meant the deeper rename too.
    Confirmed: **"Mesh Trace becomes a [family]... we select what profile
    we are sniffing."** BRAND.md's Interface Naming table revised the same
    session (own "Revised 2026-08-25" note there) — Meshtastic and MeshCore
    collapse into one family name, "Core Trace" is retired outright, Open
    Trace/Spectrum Trace are unaffected (each is a single profile with no
    sub-choice). Deliberately **not** called a "mode" in docs/code despite
    being a natural word for it in conversation — HOME_LISTEN/
    DISCOVERY_SWEEP/ENERGY_SWEEP already own "mode" (Watch/Probe/Sweep) for
    the radio's own operating state, a different axis from which network
    family is being traced; overloading the word would make the two
    impossible to talk about separately.
  - **"Green lit to implement"** — the user's explicit go-ahead once the
    mockup reached this state. Real changes, this session: `BRAND.md`
    (table + revision note), `ui_labels.h` (flat `uiProfileLabel()` ->
    `uiTraceModeLabel()`/`uiSubProfileLabel()`/`uiActiveProfileLabel()`,
    `test_ui_labels` rewritten to match), `ui_menu.h`'s `MenuAction` enum
    (`PROFILE_SWITCH` -> `SELECT_MESHTASTIC`/`SELECT_MESHCORE`,
    `test_ui_menu` updated — a synthetic DIRECT-root fixture added since no
    production root row is DIRECT any more, to keep that branch of
    `MenuState` covered), `channel_plans.h`'s `nextHomeListenProfile()`
    deleted as dead code (nothing calls a cycle-toggle once the menu picks
    a target profile directly), and the full `ui_task.cpp`/`.h` port
    detailed in the Phase 6 checklist entry above. Two things had no
    real-hardware equivalent and were adapted rather than copied literally:
    the mockup's alpha-blended RX-pulse decay became a binary hold-then-
    revert (RGB565/Arduino_GFX has no cheap alpha blending), and the
    toast's slide-in/countdown-bar animation — which needed no alpha at
    all, just per-frame rectangle geometry — is driven by a bounded ~60ms
    fast-redraw burst for its own ~1.4s lifetime rather than a continuous
    animation loop.
  - **Design artifact preserved as a living reference**, per the user's
    explicit request ("save your rolling preview artifact as a design
    document that we can build on and use later when it comes to
    enhancements"): republished with its before/after comparison intact
    and its own proposed-vs-committed sections now describing v0.6.1's
    actual shipped state, so it stays useful for scoping Phase 7/8's menu
    additions without needing a fresh mockup built from scratch each time.
    Link: https://claude.ai/code/artifact/84eb5187-9f26-4fc1-8b6b-39f9969a86ea
- **2026-08-25 — v0.6.1's "Mesh Trace" branding walked back to plain
  "Profile" (v0.6.2), same day.** Direct continuation of the entry above —
  the user reviewed the shipped v0.6.1 naming and pushed back: "I'm
  thinking the trace naming doesn't make sense. We kind of have a sniffer
  named trace that sniffs different protocols that are really LoRa
  presets." The real problem, once named: "Trace" was doing three jobs at
  once — the product name (LoRaTrace), a per-profile brand (Mesh Trace/
  Open Trace/Spectrum Trace), and a saved-session noun (a Trace) — and
  branding every profile its own "___ Trace" name made four settings on
  one receiver read like four separate products.
  - **Proposed fix, confirmed before implementing:** drop per-profile
    branding entirely; call the axis **Profile** — not a new coinage,
    already this doc's own preferred word ("Voice and Tone": "'Profile'
    instead of 'attack mode'") from before the Trace-branding detour ever
    started. Presets get their real, technical names instead of marketed
    ones: **Meshtastic**, **MeshCore**, **Reticulum**, and **Spectrum**
    (short for General Exploration, the one profile without its own
    proper noun). **Trace goes back to meaning exactly one thing: a saved
    session or run.** Menu shape barely moves — still two root groups,
    just the group label changes from "Mesh Trace" to "Profile," and
    Reticulum/Spectrum will slot in as two more flat entries in that same
    group once Phase 8 gives them a real channel table, no new nesting
    decision needed.
  - **Real changes:** `BRAND.md` (Interface Naming section rewritten with
    a "Revised again 2026-08-25" note — Mesh Trace/Open Trace/Spectrum
    Trace all retired, replaced by the flat Profile table above), a
    rewritten `ui_labels.h` (`uiTraceModeLabel()`/`uiSubProfileLabel()`/
    `uiActiveProfileLabel()` collapsed back into one flat
    `uiProfileLabel()`), `ui_task.cpp`'s `ROOT_ITEMS` (root row 0 relabeled
    "Profile," `PROFILE_GROUP_ITEMS` renamed from `MESH_TRACE_GROUP_ITEMS`,
    `drawFooterStatus()`/`drawMenuRoot()`/`fireMenuAction()` simplified to
    the flat label — no more family/sub-profile composition, since there's
    no branded family left to compose from), `ui_menu.h`'s comments (the
    `MenuAction` enum itself — `SELECT_MESHTASTIC`/`SELECT_MESHCORE` —
    didn't need to change, only what the root row is called), and
    `test_ui_labels`/`test_ui_menu` updated to match. Verified for real:
    `pio test -e native` **88/88**, `pio run -e cardputer-adv` **SUCCESS**
    (RAM 50304/327680B, flash 957645/3342336B) — see the Phase 6 checklist
    entry above for the updated numbers.
  - **Design artifact corrected again**, same republish workflow as the
    entry above: `drawMenuRootV2`'s root-row label, `drawProfileGroupV2`,
    every preset's `profile` field, and the intro/compare-section prose all
    walked back from "Mesh Trace: Meshtastic" composition to a plain
    "Meshtastic" — verified with the same Node.js DOM-stub harness
    (`dom_harness.js`) before republishing, no throws. The genuinely
    historical bits (the Phase-5 `render()`/`drawLegacy*` functions, and
    the `systemBeforeState` snapshot that predates even v0.6.0) were left
    untouched, same "don't rewrite frozen history" rule this file has
    followed since the before/after section was first built. Same link as
    above, still the living reference for Phase 7/8.

- **2026-08-25 (evening) — First real hardware bench pass of Phase 6's UI
  redesign, v0.6.2 -> v0.6.3.** Everything below came out of putting the
  Cardputer in front of the shipped v0.6.2 build for the first time — the
  redesign compiled and looked right in the mockup, but direct-to-panel
  drawing has real failure modes a build report can't catch. One version
  bump for the whole session rather than one per item: unlike the earlier
  same-day Mesh Trace/Profile revisions, these are one continuous bench
  pass with no separate decision points in between.
  - **Full-screen blink/flicker and toast-time tearing, root-caused and
    fixed twice.** First pass: `drawPage()` was unconditionally wiping the
    entire content region on *every* redraw — not just page changes, but
    the idle 1Hz tick and the toast/pulse animation's 60ms tick too — and
    `nextPage()`/`prevPage()`/`jumpToPage()` were doing a second, fully
    redundant `fillScreen()` on top of that right before `drawPage()`'s own
    wipe. Fixed by dropping the redundant page-change `fillScreen()` calls
    and giving the toast's fast-redraw burst a lightweight path that
    touched only the header + toast band instead of the full page. That
    reduced but didn't eliminate the flicker, because direct-to-panel
    drawing makes every intermediate draw call visible on the glass no
    matter how little of it changed. Real fix, same session: an off-screen
    `Arduino_Canvas_Indexed` (`.pio/libdeps/.../GFX Library for Arduino`,
    already vendored) — every draw call in `ui_task.cpp` now targets this
    buffer, and nothing reaches the panel until one `tft->flush()` blits
    the whole composed frame in a single SPI burst
    (`Arduino_TFT::drawIndexedBitmap`, confirmed against the vendored
    source: one `startWrite()`/`writeIndexedPixels()`/`endWrite()`
    sequence). `_Indexed` rather than the full RGB565 canvas: this UI only
    ever uses 6 colours, so 1 byte/pixel loses nothing and costs ~32KB
    instead of ~63KB. This reverses the "no canvas/framebuffer,
    direct-to-panel" note v0.6.1's toast-layer comment carried — real
    hardware found a real problem that design choice didn't have an answer
    for. Once the canvas made every visible frame atomic, the toast-only
    fast-redraw path from the first pass had nothing left to protect
    against and was deleted again — `fullRedraw()` now runs unconditionally
    at every cadence, animation burst included.
    **Near-miss caught before it shipped:** an earlier draft tried
    batching draws with an outer `tft->startWrite()`/`endWrite()` pair
    around `drawHeader()`+`drawPage()`. Reading the vendored
    `Arduino_HWSPI`/`SPIClass` source first showed this would have
    deadlocked on the very first nested `fillRect()` call — `fillRect()`
    already wraps itself in its own `startWrite()`/`endWrite()`, and
    `SPIClass::beginTransaction()` takes a plain, non-recursive
    `xSemaphoreTake(paramLock, portMAX_DELAY)` with no reentrancy guard.
    Never reached hardware; caught by reading the library source before
    flashing.
  - **Menu label overflow, fixed by dropping decoration rather than
    shrinking text.** `"Profile Meshtastic >"` was exactly 20 characters at
    size-2 text (12px/char = 240px, the panel's full width) — landing
    right on the wrap boundary and forcing a second line. Root row values
    now read `"Profile: Meshtastic"` (19 chars, 228px, clear margin) with
    the trailing `"> "` dropped from every root row entirely — a bare list
    is already legibly a menu, the arrow was just noise (bench feedback).
    No text-size change needed once the redundant decoration was gone.
  - **WiFi toggle toast bug, found by using the feature normally:**
    turning WiFi on showed "WiFi OFF" and vice versa. `fireMenuAction()`'s
    `WIFI_TOGGLE` case called `wifiToggle()` (which only flips a
    `apRequested` flag) and then immediately read `wifiIsEnabled()`
    (`apActive`, which `wifiTask`'s own loop on Core 0 doesn't update until
    its next pass) to build the toast — always one step behind. Fixed by
    reading the pre-toggle state and negating it, the same pattern
    `SELECT_MESHTASTIC`/`SELECT_MESHCORE` already used correctly (report
    the requested target, don't re-query the not-yet-applied live state).
  - **Trace pause/standby — a real battery lever, added after the operator
    asked how to reduce heap use and floated gating GPS and the radio
    pipeline behind toggles.** Investigation first, not assumption: all 5
    tasks' stacks sum to ~24.5KB (GPS alone is 3072B) — not a real heap
    lever next to WiFi's AP (~55-56KB, already measured, already gated) or
    this session's own new canvas (~32KB). And GPS power turned out not to
    be independently gateable at all: `io_expander.h` documents IO-expander
    pin P0 doing double duty — antenna switch *and* GPS power on one line —
    so a GPS-off toggle would also deafen the radio. What was left
    standing: a genuine pause for the radio-listening + logging pipeline,
    which does have a real power story (SX1262 continuous RX vs. its sleep
    mode). Confirmed with the operator up front: GPS stays running
    untouched while Trace is paused, so position is already fresh the
    instant it resumes.
    - `radio_task.cpp` gained a second one-slot mailbox
      (`pauseQueue`/`radioRequestTracePause()`/`radioIsTracePaused()`),
      mirroring the existing `profileSwitchQueue` pattern exactly. Pausing
      calls `radio.sleep(true)` (RadioLib's warm sleep — retains config, no
      re-`begin()` needed to resume); resuming calls `radio.startReceive()`
      directly, which wakes a warm-sleeping SX126x via the SPI command
      itself (confirmed in RadioLib's `SX126x_commands.cpp`, not just
      assumed). The existing 5s liveness-timeout branch had to be guarded
      too — it was unconditionally calling `radio.startReceive()` as a
      missed-interrupt safety net, which would have silently undone the
      sleep every 5 seconds. A profile switch arriving while paused always
      wins and resumes listening, on the theory that picking a different
      protocol is an active operator choice that shouldn't be silently
      swallowed by a stale pause — **confirmed on real hardware the same
      session**: switching Meshtastic->MeshCore while Trace was in standby
      correctly woke the radio and retuned it.
    - Shipped first as a `System` group item, then **promoted to its own
      root-level DIRECT row the same session**, above Profile, on operator
      feedback that it's central enough to fire without drilling into a
      group first. Root label reads `"Trace: Active"`/`"Trace: Standby"`.
      Named "Trace" after a naming check, not by default: "MeshTrace" was
      rejected outright (revives the exact branding this doc's own earlier
      2026-08-25 entries walked back). Plain "Trace" was flagged too —
      BRAND.md pins "Trace" to exactly one meaning, a saved session/run —
      and kept anyway as a deliberate, narrow exception: the saved-session
      noun is always countable ("a Trace," "your saved Traces") while the
      live-toggle usage always pairs with a state word (Active/Standby) and
      never stands alone, so the two don't actually collide in practice.
      BRAND.md's Interface Naming section now documents this exception
      explicitly rather than leaving it as a silent drift.
    - RADIO page shows a `STANDBY` banner when paused, in the gap below the
      hero column the layout otherwise leaves empty — rx/log/drop stay
      visible as real frozen totals rather than being hidden, since the
      thing that needed to stop being ambiguous was "is the radio listening
      right now," not the counters themselves. `gps_task.cpp`,
      `io_expander.cpp`, and `logger_task.cpp` deliberately untouched — see
      above for GPS, and the logger already does the right thing for free
      (queue sits empty while paused, `session.csv`'s health row keeps
      going regardless, which incidentally leaves a natural audit trail of
      standby duration).
  - **Heap bar flipped to show usage, not remaining space, with a real red
    tier.** The SYSTEM page's heap bar used to fill with *free* heap and
    empty toward the danger zone — visually backwards next to a colour that
    was getting more alarming as the bar got shorter. Now fills with
    *used* heap against the same ~512KB no-PSRAM budget (DESIGN.md §1), so
    bar direction and colour escalate the same way. One shared
    `heapUsageColour()` (green under 80% used, yellow 80-90%, red above
    90%) now drives the bar, the "k heap" text, and the header's heap
    status dot — previously three separate colour computations, one of
    them a flat 2-tier threshold with no red tier at all.
  - **The web dashboard had the same ambiguity RADIO's STANDBY banner just
    fixed on-device.** `/api/status` exposed `rx`/`crc_err`/etc. but
    nothing saying whether the radio was actually listening — someone
    watching the dashboard during a Trace pause would see frozen counters
    with no explanation. Added a `trace_paused` field to the JSON and a
    `Trace: ACTIVE`/`STANDBY` card at the top of the dashboard's radio
    stats, using `radioIsTracePaused()` (already public from the pause
    work above).
  - **`wifi_task.cpp`'s per-request heap churn, fixed while already
    touching this file.** `handleRuns()` built its JSON with `String`
    concatenation in a loop over every run directory (`json += String(idx)`
    per entry) — real reallocation/copy churn on every dashboard poll,
    worse the more field sessions accumulate. Replaced with a fixed 2KB
    stack buffer + `snprintf`, matching every other handler in this file;
    a defensive cap (~340 runs before truncation) beats a buffer overrun,
    and `detections.csv` stays reachable directly off the SD card
    regardless of what this endpoint lists. `streamCsvFile()`'s
    `Content-Disposition` header — three chained `String` allocations for
    one request — got the same treatment.
  - **Test suite:** `test_ui_menu` restructured for the new 3-root shape
    (Trace/Profile/System) and Trace's move from a System group item to a
    root DIRECT row mid-session — **88/88** `pio test -e native`, same
    total as before this session (Trace's coverage moved, it didn't add a
    net-new test). `pio run -e cardputer-adv` **SUCCESS** (RAM
    50312/327680B, flash 961325/3342336B), both actually run, not assumed.
    Bench-verified on the attached Cardputer at multiple points through
    the session, not just at the end: the canvas fix, the menu fixes, and
    the Trace pause/resume + profile-switch-while-paused interaction were
    all confirmed working on real hardware before being called done.
  - **Version: v0.6.2 -> v0.6.3.** PATCH-level — bench-pass fixes plus one
    new operator-facing toggle inside Phase 6's existing UI scope, not a
    new build-order phase.

- **2026-08-25 (later still) — Real backlight brightness control, v0.6.3 ->
  v0.6.4.** The v0.6.3 Brightness menu (4 fixed presets) was built on a
  plain `digitalWrite(HIGH)` backlight — worked as an on/off toggle, no
  actual dimming behind it. Wiring up real PWM (`backlight.h`/`.cpp`,
  ESP32 LEDC) surfaced a genuine hardware bug on the first bench pass, not
  just missing polish.
  - **The bug, in the order it was found:** first attempt (20kHz, 8-bit,
    linear `pct * 255 / 100` duty) — 50% and 75% drove the display fully
    black, while 25%(untested)/idle's 15%/boot's 100% were fine. Dropped to
    1kHz on the theory that 20kHz's 50µs period was too fast for whatever
    backlight driver circuit is on this Cap to track cleanly at partial
    duty — this fixed 50%/75%, but then **25% broke at 1kHz**, right after
    50/75 had just started working there. That pattern (a *lower* duty
    failing while higher ones newly worked) doesn't fit a simple "wrong
    frequency" story, and mid-session the display got stuck fully off and
    stopped responding to further `ledcWrite()` calls entirely (even
    re-writing the already-proven-good 100%) — needed a real power cycle to
    recover, not just a different duty value, which reads like the driver
    circuit's protection/fault-latch tripping under an out-of-spec pulse
    pattern rather than a simple software off-by-one.
  - **Root cause, found by reading the reference this project cited but
    hadn't actually opened for this:** the operator pointed at M5PORKCHOP
    (github.com/0ct0sec/M5PORKCHOP) for how a real M5Stack project handles
    brightness. It doesn't hand-roll PWM at all — it calls
    `M5.Display.setBrightness()` (M5Unified -> M5GFX). Traced one level
    further into M5GFX's own board-autodetect code
    (`M5GFX.cpp`, `board_M5CardputerADV` branch — this project's exact
    board) and found the real, validated parameters for this precise GPIO:
    `_set_pwm_backlight(GPIO_NUM_38, ch, /*freq=*/256, /*invert=*/false,
    /*offset=*/16)`. GPIO 38 is `PIN_TFT_BL` — not a generic reference,
    *this* hardware. `LovyanGFX`'s `Light_PWM::setBrightness()`
    (`src/lgfx/v1/platforms/esp32/Light_PWM.cpp`) uses that `offset` in a
    genuinely non-linear duty curve (9-bit resolution, a rounding term, an
    `offset`-scaled floor baked into the formula) specifically so low/mid
    brightness values stay above whatever duty this class of backlight
    driver needs to stay in regulation — a naive linear map has no such
    floor, which is exactly the "some values collapse, not a clean
    threshold" symptom hit twice. `backlight.cpp` now replicates that
    formula and those constants directly (256Hz, 9-bit, `offset=16`,
    non-inverted) rather than a third guess.
  - **Confirmed on real hardware, live, not just after the fix**: a
    background serial monitor watched `[backlight] set: pct=N duty=D/511`
    diagnostic lines (added specifically for this investigation, kept
    afterward) while the operator cycled brightness levels in real time —
    both during the failing 20kHz/1kHz attempts (to pin down the actual
    duty values involved before guessing further) and after the M5GFX fix,
    where all 4 original presets (25/50/75/100%) came back working. The
    5-20% range is new territory this session opened up (the old fixed-
    preset menu never went below 25%) and is flagged as needing its own
    bench sweep, not assumed safe just because the formula's intent is to
    keep low values working.
  - **Two operator-requested upgrades landed the same evening**, once the
    PWM was trustworthy:
    - **Brightness slider** (5-100% in 5% steps) replacing the 4 fixed
      presets — `ui_menu.h` gained a real `SLIDER` `ItemKind` (NEXT/PREV
      fire `sliderIncrease`/`sliderDecrease` once entered, applied live
      every step, no list to navigate). **Idle-dim timeout became
      configurable** (Off/30s/60s/2min/5min, cycled from a menu row)
      instead of the hardcoded 60s the toggle-only version shipped with
      earlier in the session.
    - **Both persist to SD** — `display_settings.h`/`.cpp`, a new,
      narrowly-scoped module (`/loratrace/display.txt`) rather than folding
      into `config.h` (that file's own header comment already states its
      scope as deliberately narrow, "not the general Logger/settings
      architecture Phase 2 will eventually own" — display settings are
      exactly what that carve-out was for). Mirrors `config.cpp`'s
      `writeProfileConfigToSD()` pattern exactly: bounded (2s) `SpiBusLock`
      wait, fail-safe defaults on a missing/invalid file. This is
      `ui_task`'s first-ever SD access — every other on-device menu action
      all session (profile switch, WiFi/Debug toggle, Trace pause) had been
      runtime-only; only the web UI's settings save had ever written to SD
      from a running task before this. Brightness saves once on BACK-out of
      the slider (not every step — would hammer the card if held down);
      idle-dim timeout saves on each cycle press (a discrete, deliberate
      tap, not a scrub). Idle-dim's floor is now `min(15%, the operator's
      active level)` instead of a fixed 15% — needed once brightness could
      go below that, so idle-dimming can't make the screen *brighter* than
      a deliberately low active level.
  - **The menu gained real nesting, on direct operator request**: "System >
    Display > Brightness/Idle dim" replaces Brightness living at root and
    Idle-dim living flat in System. `ui_menu.h`'s `MenuState` had been an
    explicitly two-level design since the Phase 6 redesign ("root, then one
    group inside it, never more," stated in its own header comment) — a
    GROUP nested inside a GROUP is a genuine third level, not something a
    special case for one screen was worth building given this is the
    *second* time a nesting need has come up this session (System's own
    flat WiFi/Debug/Trace grouping was the first). Generalized instead:
    `RootEntry`/`MenuEntry` (two separate, differently-shaped types)
    collapsed into one recursive `MenuItem` (ACTION/GROUP/SLIDER,
    `items`/`itemCount` pointing at more of the same type), and `MenuState`
    became a small depth-bounded stack (`MAX_DEPTH = 4`, only 3 actually
    used: root -> System -> Display) instead of separate `rootIdx_`/
    `groupIdx_` fields. `ui_task.cpp`'s drawing code got simpler as a side
    effect, not more complex: `drawMenuRoot()`/`drawMenuGroup()` (two
    near-duplicate functions, one per hardcoded depth) collapsed into one
    generic `drawMenuList()` that works at any depth, plus `drawMenuSlider()`
    unchanged in shape. The header breadcrumb now walks the full ancestor
    chain (`menu.breadcrumbLabel(i)` for `i` in `[0, breadcrumbCount())`)
    instead of showing just one group name — e.g. "MENU > System > Display
    > Brightness" once inside the slider.
  - **Test suite:** `test_ui_menu` rewritten for the recursive model —
    covers a GROUP opening a nested GROUP, a SLIDER reached from two levels
    deep, and BACK walking up exactly one level at a time (Display ->
    System -> root -> closed) rather than jumping straight to closed.
    **90/90** `pio test -e native`, `pio run -e cardputer-adv` **SUCCESS**
    (RAM 50328/327680B, flash 964425/3342336B), both actually run.
    Bench-verified on the attached Cardputer after flashing, operator
    confirmed working before this entry was written.
  - **Version: v0.6.3 -> v0.6.4.** PATCH-level — one bug fix (backlight PWM,
    arguably severe enough to have blocked calling Brightness "done" at
    v0.6.3 had it been caught sooner) plus operator-requested UI/persistence
    upgrades, all inside Phase 6's existing UI scope, not a new build-order
    phase.

- **2026-08-25 (later still) — Docs/code cleanup pass and `ui_task.cpp`
  split, requested by the operator after asking whether the codebase was
  "becoming a monolith" ahead of Phase 7/8.** A survey (file sizes, stale
  comments, dead code, doc-vs-code drift) found `ui_task.cpp` at 1265
  lines — 2.75x the next-largest file — mixing six largely-independent
  concerns: page drawing, menu/toast rendering, keyboard-to-action decode,
  cross-task business logic, animation timing, and canvas lifecycle. Given
  ROADMAP.md's own Phase 7 plan has `DISCOVERY_SWEEP` land as "additional
  entries in Phase 6's grouped menu" — i.e. more code into this same file —
  splitting now, before that lands, was judged cheaper than splitting a
  larger file later.
  - **Small fixes first:** CLAUDE.md's "Proposed layout" tree was missing
    five real file pairs (`backlight.*`, `battery.*`, `display_settings.*`,
    `run_log.h`, `session_log.h`) and five test dirs — added. `main.cpp`
    hardcoded `"phase 5"` into the boot-serial banner and splash text while
    the device was actually on Phase 6/v0.6.4 — the exact class of mismatch
    this project's own version-provenance discipline exists to prevent;
    fixed by dropping the redundant literal entirely (it was never derived
    from `FIRMWARE_VERSION`) rather than just bumping it to "6," which would
    only recreate the same drift next phase. `board_pins.h`'s display-pins
    `TODO(verify)` was already resolved (Phase 1 hardware-verified
    2026-08-23) but never removed — updated to say so.
  - **A real bug caught mid-cleanup, not just theoretical:** the initial
    survey flagged `ui_task.cpp`'s `#include "detection.h"` as dead (no
    grep hits for `Detection`/`detectionFormatCsv`/`missionProfileName`).
    It was removed, and a `pio run -e cardputer-adv` baseline still
    succeeded — but only because `detection.h` was still reaching the file
    transitively through `radio_task.h`. Re-checking before the split
    surfaced the actual bug: `drawGpsPage()` calls
    `detectionFormatTimestamp()` directly, which the original grep pattern
    never matched. Restored the include immediately, before it could get
    buried inside the split. Lesson logged here on purpose: a build passing
    is not proof an include is unused when a transitive path exists —
    ROADMAP.md's own "verify, don't assume" discipline applies to include
    hygiene too, not just hardware claims.
  - **PlatformIO was not installed in this session's environment** (every
    prior session's own notes on this — "no pio in this environment" — held
    true here too, initially). `pip install platformio` worked cleanly
    against this session's network access, giving a real `pio run -e
    cardputer-adv` / `pio test -e native` toolchain for the whole session
    rather than the "reviewed by inspection, not compiled" fallback this
    project has repeatedly had to fall back on. Worth remembering for a
    future session that hits the same "no pio" wall: try installing it
    before assuming inspection-only review is the ceiling.
  - **The split:** `ui_task.cpp` (1265 lines) became four files —
    `ui_task.cpp` (484 lines: task lifecycle, keyboard input decode, the
    main loop, and every piece of shared operator-facing state, since this
    file owns seeding it from SD at boot and persisting it back),
    `ui_pages.cpp` (712 lines: every `drawHeader()`/`drawPage()`/status-page/
    menu/toast drawing function), `ui_actions.cpp` (130 lines: just
    `fireMenuAction()` — the radio/WiFi/logger/backlight/SD calls a fired
    menu row makes), and a new private header `ui_task_shared.h` (93 lines)
    declaring the `extern` state and the handful of functions
    (`drawHeader()`, `drawPage()`, `fireMenuAction()`, `showToast()`,
    `toastActive()`, `rxPulseActive()`) that genuinely cross the three
    `.cpp` files — not part of `ui_task.h`'s own public two-function API,
    which is unchanged. Mechanical move, not a rewrite: comments preserved
    throughout, only two needed fixing because they said "below" referring
    to a function that moved to a different file (`fireMenuAction()`'s and
    `drawToast()`'s own references to `uiTask()`).
  - **A real linker bug the split itself caused, caught immediately by the
    build, not missed:** giving `ui_task.cpp`'s internal `tft` pointer
    external linkage (needed so `ui_pages.cpp` could draw to it) collided
    with `main.cpp`'s own pre-existing global `Arduino_GFX *tft` (the raw
    boot-splash panel handle) — invisible before the split because
    `ui_task.cpp`'s `tft` had internal (anonymous-namespace) linkage the
    whole time. `ld` caught it immediately: "multiple definition of
    `tft`." Fixed by renaming ui_task's own copy to `uiTft` throughout the
    three split files (a plain `sed` rename, then a clean rebuild + full
    test run to confirm nothing else broke). Left as a documented example
    of exactly why "reviewed by inspection" is a weaker guarantee than a
    real compile — this specific bug was invisible to manual code reading
    ahead of time and only surfaced because a real toolchain was available
    this session.
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS** — RAM
    50328/327680B, byte-for-byte identical to the pre-split baseline; flash
    964437/3342336B, +180B over the pre-split baseline (964257B after the
    small fixes), an expected, negligible cost of cross-translation-unit
    calls that could previously inline within one file. `pio test -e
    native` **90/90**, unchanged (`ui_task.cpp`/`ui_pages.cpp`/
    `ui_actions.cpp` were never part of the host-native suite — they need
    Arduino.h). A one-off pass with `-Wall -Wextra` (not a permanent
    `platformio.ini` change) found zero new warnings from any of the four
    split-related files or `main.cpp`; the two warnings that pass surfaced
    are both pre-existing, in files this session never touched
    (`detection.h`, `gps_task.cpp`).
  - **What this is not:** a hardware bench pass. Every check above is a
    real compile/link/test run, not "reviewed by inspection" — genuinely
    stronger evidence than most of this project's non-hardware-verified
    changes get — but `ui_task.cpp`/`ui_pages.cpp` are still the display/
    input subsystem, and this project's own standing rule is that a
    compiling build says nothing about whether real glass renders
    correctly. Nothing here changed behavior on purpose, and the RAM number
    matching the pre-split baseline exactly is a good sign, but the next
    hardware session should still re-run the same panel/menu/input checks
    Phase 6's own bench passes already established (carousel paging, all
    three menu levels including the Brightness slider, Trace pause/resume,
    idle-dim) before trusting this at the same confidence level as
    v0.6.2-v0.6.4's own hardware-confirmed work.
  - **Version: unchanged, v0.6.4.** No behavior change and no new
    build-order scope — a structural cleanup pass, not a phase item or a
    functional patch.

- **2026-08-25 (later still) — Boot mark: BRAND.md's unbuilt logo concept
  finally built, replacing the plain-text boot splash.** The operator
  asked to make the boot splash "more dynamic and fun," pointing out it
  reads "like an old static BIOS." BRAND.md already had an unbuilt concept
  for exactly this — "an L-shaped path that transitions into three signal
  arcs" — but its own tone guardrails (calm, instrument-like, not a
  "hacker toy," no neon/consumerized aesthetics) sit in real tension with
  "fun," so that was surfaced directly via `AskUserQuestion` before
  building anything: tone ("polished, still restrained" — chosen),
  content (BRAND.md's own arc concept — chosen), motion ("fuller
  sequence" — chosen), and whether to mock it up visually first before
  touching firmware, given this project has no display simulator (yes —
  chosen, same discipline as Phase 6's own mockup workflow).
  - **Mockup, "Signal Acquired"** (https://claude.ai/code/artifact/e6e635f3-f5af-4d2a-8eab-549de61a8e20):
    a canvas simulation at real 240x135 device resolution, RGB565-
    quantized colour, two takes on the arc reveal (Take A: all three arcs
    together, "radar ping"; Take B: arcs resolve one at a time with an
    amber "lock" flash each, "sequential acquire"). Operator picked
    **Take B**, with one refinement: the diagnostic log text (previously
    flush-left at the panel's edge) should align under the mark's first
    arc instead — implemented as `LOG_X = ANCHOR.x + ARC_RADII[0]`, both
    in the mockup and, after, in real firmware.
  - **Real implementation, `main.cpp`:** a new `playBootMark()` replaces
    the two old `splashLine()` calls that used to print "LoRaTrace RX" /
    "vX.Y.Z" as plain text. Procedural — `drawLine()`/`fillArc()`/
    `fillCircle()` calls against a handful of coordinate constants
    (`MARK_ANCHOR_*`, `MARK_PATH_*`, `MARK_ARC_RADII`), not a bitmap —
    still direct-to-panel, still one-shot inside `initDisplay()`/`setup()`
    before any task starts, same model the splash has always used; no
    canvas, no interaction with Phase 6's flicker fix (that fix exists for
    a *redraw loop*, and this is a short sequence that runs once). Two new
    colours, `SPLASH_GREEN`/`SPLASH_AMBER` (0x4D0E/0xDD84), deliberately
    distinct from `ui_pages.cpp`'s `COL_GOOD`/`COL_WARN` — a one-shot boot
    accent shouldn't borrow meaning from an on-device status colour. No
    alpha (RGB565 has none, same constraint the toast/RX-pulse work
    already documented) — the amber-to-green "lock" is a hard colour
    swap, not a fade, and the wordmark/version lines just appear, same as
    every other line in this splash always has.
  - **Real engineering deltas from the mockup, found only by actually
    building it, not assumed:**
    - **Geometry rescaled down.** The mockup's first-pass proportions (a
      mark spanning ~88px of the panel's 135px height) were sized without
      checking them against the real diagnostic log's full line count —
      up to 7 lines in the success path (IO expander, config, GPS task,
      logger task, freq/SF, BW/CR/sync, and a conditional WiFi SSID
      line). At that size the mark would have pushed the longest real
      sequence off the bottom of the panel. Shrunk the mark to fit in the
      top ~44px (`MARK_ANCHOR_Y=24`, `MARK_ARC_RADII={6,10,14}`, vs. the
      mockup's original `{10,17,24}` around `y=60`), leaving the log its
      original ~90px of room — comfortably fits even the 7-line case.
      Mockup corrected to match, not left showing stale proportions.
    - **Angle convention, verified against the vendored source, not
      assumed.** Arduino_GFX's `fillArc()`/`drawArc()` measure degrees
      from 12 o'clock, clockwise (confirmed by reading
      `writeFillArcHelper()` in the vendored `Arduino_GFX.cpp` — the same
      well-known TFT_eSPI-derived scanline ring-fill algorithm, not
      guessed from the function signature alone). The mockup's canvas
      arcs used the browser canvas convention (0°=3 o'clock, clockwise)
      instead. Converting is a fixed +90° offset between the two
      conventions (not a tuned/guessed value) — `MARK_ARC_START_DEG`/
      `MARK_ARC_END_DEG` (45.3°/134.1°) are exactly the mockup's
      `ARC_START`/`ARC_SWEEP` radians converted through that offset, so
      the arcs open the same direction on real glass as in the approved
      preview.
    - **Flash cost measurably higher than first guessed.** The code
      comment originally claimed "a few hundred bytes" (reasoning that
      procedural drawing is cheap); the actual measured delta is **+6.0KB**
      (970441B vs. 964437B, `pio run -e cardputer-adv`) — because this is
      the first call anywhere in the firmware to `fillArc()`/`drawArc()`,
      so most of that 6KB is the arc-fill scanline math (and its float
      sin/cos/fmodf) linking in for the first time, not a per-call cost.
      Comment corrected to the measured number rather than left wrong —
      still trivial against a 3.34MB partition at 29% used. RAM: +4B.
    - **FATAL visibility preserved deliberately, not by accident.** A new
      `splashX` global (mirrors the existing `splashY` pattern) defaults
      to the old flush-left `4` and is only moved to the arc-aligned
      position by `playBootMark()` once the mark has actually drawn — so
      a FATAL firing before the mark plays (or if `initDisplay()` itself
      failed) still reads exactly as it always has, not at a
      not-yet-established new margin.
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS** (RAM
    50332/327680B, +4B; flash 970441/3342336B, +6004B), `pio test -e
    native` **90/90** unaffected (main.cpp isn't part of the host-native
    suite), a `-Wall -Wextra` pass found zero new warnings. **Not yet
    bench-tested on real hardware** — this is direct-to-panel drawing on
    the one part of this project's history that has *never* gotten a
    layout right on the first try without a real bench pass (the original
    2026-08-22 IPS-offset bug, and Phase 6's own flicker/tearing only
    found once real glass was in front of it). The angle-convention read
    and the rescaled-geometry fit are both reasoned carefully against
    source and real line counts, not guessed — but per this project's own
    standing rule, that is still "reviewed by inspection," not proof the
    arcs land where intended or that the log fits cleanly on real glass.
    Next bench session: confirm the arcs open toward 3 o'clock as
    intended (not mirrored/rotated), confirm all 7 log lines are legible
    and none clip the bottom edge, and time the actual ~1.7s added to
    boot.
  - **Version: unchanged, v0.6.4.** A boot-splash visual change, not new
    build-order scope.

- **2026-08-25 (even later still) — Boot mark, round 2: the diagnostic log
  trimmed to a real 3-line hardware checklist, freeing room for a bigger
  wordmark.** The operator asked directly: "do we need all 7 lines... what
  dictates a successful boot?", naming SD/IO-board/GPS/LoRa as the things
  that actually matter. Investigated each of the 7 original lines against
  the real code (`gpsTaskStart()`/`radioTaskStart()` in `gps_task.cpp`/
  `radio_task.cpp`) rather than assuming the splash text already meant
  what it said, and surfaced a real finding before touching anything:
  - **`gpsTaskStart()` doesn't check the GPS hardware at all** — it only
    creates a mutex and spawns a FreeRTOS task; it never talks to the GPS
    module. The old "GPS task: started" line would read identically for a
    working GPS and a dead/unpowered one. Relabeling it "GPS: OK" would
    have been a check that doesn't check anything — the opposite of what
    this trim was trying to do. Surfaced directly via `AskUserQuestion`
    (a real check with ~1.5-2s added latency to wait for a first NMEA
    sentence, vs. keeping it honestly informational, vs. dropping it) —
    **operator chose to drop GPS from the checklist entirely**, since fix
    status is already a glance away on the GPS page moments after boot.
  - **Radio and IO expander were already real checks** — `radioTaskStart()`
    calls `radio.begin()`, a genuine SPI transaction with the SX1262, and
    `ioExpanderInit()` does real I2C writes to the antenna-switch chip,
    both already `fatal()`-gated on failure. Just needed an explicit
    success-path splash line for radio (one never existed before this).
  - **SD had a real, separate gap**, not just a relabeling opportunity:
    `loadProfileOverridesFromSD()`'s one bool return means "was an
    override applied" — a missing SD card and a mounted card with no
    config file both report the same "Config: default" on the old splash,
    genuinely indistinguishable without reading serial. Fixed with a new
    optional `bool *sdMounted` out-parameter (single call site, low risk)
    exposing `SD.begin()`'s own result separately from the
    applied-override count.
  - **Second `AskUserQuestion`, confirmed dropping the rest**: freq/SF/
    BW/CR/sync detail, the config-source line, and the WiFi SSID line all
    move off the splash — still in serial always, and on CHANNEL/SYSTEM
    once the UI starts (the WiFi SSID specifically is what the
    WiFi-toggle toast already shows the moment it's actually needed).
    Also dropped on the same reasoning as GPS, without a separate ask
    since it followed directly from the operator's own stated principle:
    "Logger task: started," which — like the old GPS line — only confirms
    RTOS resource allocation, not a hardware check, and whose only
    failure mode is already `fatal()`-gated (reaching the next line
    already proves it succeeded).
  - **Layout redesigned, not just shortened.** 3 lines instead of 7 frees
    real height (checklist now starts at y=88 instead of y=46) — spent on
    the wordmark rather than left blank: moved from squeezed beside the
    arcs at size 2 to its own full-width band below the mark at size 3
    ("LoRaTrace RX," 216px, fits at x=4 with 20px to spare — would have
    overflowed the 240px panel from its old beside-the-mark position at
    that size, which is why it moved rather than just grew in place).
    Mockup updated to the new proportions and republished at the same
    link before implementing, same discipline as round 1.
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS** — RAM
    50332/327680B (unchanged from round 1), flash **969193/3342336B**, a
    **1.25KB decrease** from round 1's 970441B: fewer `String`-
    concatenating `splashLine()` calls (the freq/BW/SSID lines each built
    a temporary `String`) outweighed the new `bool` out-param and SD
    check. `pio test -e native` **90/90** unaffected, `-Wall -Wextra`
    found zero new warnings. **Still not bench-tested on real hardware**
    — same standing caveat as round 1's entry; this round changes the
    exact same direct-to-panel layout that's never gotten it fully right
    without a bench pass. Next session should additionally confirm the
    size-3 wordmark doesn't clip at the right edge and that "SD: MISSING"
    actually shows red on a card-pulled boot, not just "SD: OK" on the
    happy path every session so far has tested.
  - **Version: unchanged, v0.6.4.**

- **2026-08-25 (even later still) — Boot mark, round 3: wordmark moved
  back beside the mark, reversing round 2's size-3/full-width layout.**
  Direct operator feedback: round 1's original beside-the-arcs placement
  "was perfect," and now that the log is down to 3 real hardware-check
  lines (round 2), the space problem that motivated moving the wordmark
  in the first place no longer exists — round 2 solved a problem round 2
  itself had already made moot by the time the wordmark change landed.
  Reverted `playBootMark()`'s wordmark/version back to size 2, positioned
  beside the mark (`MARK_ANCHOR_X + MARK_ARC_RADII[2] + 6, 14`/`32`, the
  exact round-1 coordinates), and `MARK_LOG_Y` back to 46 (from round 2's
  88) — the 3-line checklist has always fit comfortably there regardless
  of which wordmark layout is above it. Mockup reverted to match and
  republished at the same link before implementing.
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS** — RAM/flash
    both unchanged from round 2 (50332B/969193B) — expected, since this
    is a pure constant/coordinate change (`setTextSize(3)`->`(2)`,
    repositioned `setCursor()` calls), not new code. `pio test -e native`
    **90/90** unaffected, `-Wall -Wextra` found zero new warnings. Still
    not bench-tested on real hardware — same standing caveat as rounds 1
    and 2.
  - **Version: unchanged, v0.6.4.**

- **2026-08-25 (even later still) — Boot mark, rounds 4/5: bigger icon,
  diagonal-foot path, and a signal-trace flourish, all reviewed in the
  mockup before touching `main.cpp`.** With the log at its round-2/3 size
  (3 lines) the panel had real unused space below the mark — flagged
  directly from an operator screenshot ("what can we do with all this
  space? can we add some more dynamic loading with the signal trace?").
  Round 4 grew the mark ~1.5x (`MARK_ARC_RADII` 6/10/14 -> 9/15/21,
  `MARK_ANCHOR_X/Y` 26/24 -> 30/28) and briefly split the wordmark across
  two lines ("LoRaTrace" at size 3, "RX" on its own line at size 2) to
  let the primary word hit size 3 — the full "LoRaTrace RX" string is
  216px at size 3, and no icon size leaves that much room free beside it
  on a 240px panel, so a single line at size 3 was never going to fit.
  Direct feedback the same session ("thats a lot of space between it and
  RX so I don't see how the two wouldn't fit on that line") walked the
  split back (round 5): reverted to one line at size 2 (144px), keeping
  round 4's one real improvement — "RX" drawn in `SPLASH_GREEN` rather
  than white, tying its colour to the mark. That feedback also caught a
  genuine mockup-only bug: the canvas preview had assumed a naive
  6px/char advance to position "RX" right after "LoRaTrace ", which is
  exactly correct for real firmware's Arduino_GFX bitmap font but wrong
  for the mockup's JetBrains Mono webfont — left a visible gap in the
  screenshot that made "there's room for size 3" look true when the real
  slack (~37px at size 2, against ~72px size 3 would need) said
  otherwise. Fixed in the mockup with real `ctx.measureText()`, confirmed
  by re-rendering headless before and after (`playwright-core` +
  `page.waitForTimeout()`, needed because raw `chrome --headless
  --screenshot --virtual-time-budget` doesn't reliably let a
  `requestAnimationFrame`-driven animation settle). A second reported
  artifact — a black box near the bottom right of one screenshot — could
  not be reproduced in any clean headless render; reported honestly as
  unreproduced rather than claimed fixed, with a stale-cache hard-refresh
  suggested as the likely explanation. Real firmware needs none of the
  mockup's width workaround: `tft->print()` continues from wherever the
  cursor actually ended up after the previous call, so "LoRaTrace "/"RX"
  is just two consecutive `print()` calls with two `setTextColor()`s
  between them.
  - **The path grew into a real "diagonal foot" shape.** The operator
    described the longer path two ways in one message ("start at the
    very bottom," "cut diagonally to the left right before the bottom")
    that read as two different shapes rather than two phrasings of one —
    built both as a real toggle in the mockup (`PATH_STRAIGHT` vs.
    `PATH_FOOT`) instead of guessing, then got an explicit pick in a
    follow-up ("the diagonal foot is the move"). Shipped as a 3-segment
    path — `(6,130)` near the bottom edge, a kick to `(14,108)`, straight
    up to the elbow at `(14,32)`, then into the anchor — replacing round
    1-3's flat 2-point path that stopped at a short accent stroke well
    above the panel's bottom edge.
  - **New: a signal-trace flourish fills the panel's lower third.** A
    fixed 12-point jagged sample pattern (`TRACE_PATTERN`, not real
    randomness — a fixed pattern reads as consistent "signal" frame to
    frame rather than noise) rotated through 7 discrete frames
    (`drawSignalTraceFrame()`, `TRACE_FRAME_MS = 110`), each frame a
    `fillRect()` band-clear plus 12 `drawLine()` segments, ending with a
    dim baseline (`SPLASH_GREEN_DIM = 0x2AC8`, quantized from `#2d5940`,
    same muted-green family as `SPLASH_GREEN` but visually receded) so
    the trace has a reference line to read against. Same "a few discrete
    steps, not a continuous loop" approach the arcs already use, for the
    same reason: no per-frame interpolation cost, no alpha to fade
    through. **Deliberately ambient, not a progress bar** — it plays
    entirely inside `playBootMark()`, before the real hardware-check
    lines (`SD: OK`, `IO expander: OK`, `Radio: OK`) even print later in
    `setup()`, so it does not and should not claim to track their
    progress; it's a "still listening" flourish, not literal loading
    feedback. This was an open question after the mockup pass — the
    mockup's own notes already leaned this way, and nothing since gave a
    reason to move it later in `setup()` instead, so it shipped
    self-contained as designed.
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS** — RAM
    **50332/327680B, byte-for-byte unchanged** from round 3, flash
    **969529/3342336B, a 336B increase** from round 3's 969193B — a much
    smaller delta than rounds 1-3's multi-KB moves despite visibly more
    geometry and a new function, because `fillArc()`/`drawLine()`/
    `fillRect()` were already linked in from earlier rounds; this pass
    added coordinates and a small loop, not new library machinery.
    `pio test -e native` **90/90** unaffected (`main.cpp` isn't part of
    the native env). A direct `-Wall -Wextra` compile of `main.cpp`
    alone (matching `platformio.ini`'s real flags) found zero warnings.
    Mockup already reflected this design before implementation (the
    "living reference" convention from round 1 onward) and needed no
    further changes to match what shipped. **Still not bench-tested on
    real hardware** — same standing caveat as every round of this boot
    mark: a compiling build has never been proof this exact
    direct-to-panel layout is correct on real glass, and this round adds
    the most geometry of any round so far (longer path, bigger arcs, a
    new animated region). Next session should specifically confirm the
    diagonal foot's `(6,130)` start doesn't clip the panel's bottom edge
    and that the trace band's `fillRect()` clears cleanly against the
    real ST7789V2 without any residual artifact, given the "black box"
    report earlier this round that couldn't be reproduced in the mockup.
  - **Version: unchanged, v0.6.4** (cosmetic boot-sequence polish, same
    reasoning as rounds 1-3 — no new phase scope).
- **2026-08-25 (even later still) — v0.6.5: fixed a warm-reboot flash of
  the previous session's page before the boot mark.** Operator report,
  photo-confirmed: rebooting the device (photo shows `GNSS`/`LoRa`
  Cardputer-Adv/StampS3A hardware, not a firmware label — separate from
  the actual bug) briefly showed whatever `ui_task` had last drawn before
  the boot mark's black background and arcs took over, described as
  "temporarily shows the radio cards info then does the boot loader."
  - **Root cause: ordering, not corruption or a stale framebuffer bug in
    the new v0.6.3 canvas.** The ST7789 panel's GRAM survives a warm
    reset — the ESP32-S3 resets, but the panel's own supply rail doesn't
    get power-cycled by an EN/software reset — so whatever frame `ui_task`
    last rendered is still physically sitting on the glass the instant
    `setup()` starts. `main.cpp`'s `initDisplay()` was calling
    `backlightInit()` (drives the LEDC-PWM backlight straight to 100%,
    `backlight.cpp`) *before* `tft->begin()`'s panel wake sequence and the
    boot-time `tft->fillScreen(SPLASH_BG)` had actually overwritten that
    frame with black. The ST7789 datasheet's own sleep-out sequence needs
    a real settle delay after `begin()`'s reset pulse, so there was a
    genuine multi-frame window where the backlight was already on and the
    old page was still the only thing in GRAM.
  - **The fix:** reordered `initDisplay()` — `tft->begin()` and
    `tft->fillScreen(SPLASH_BG)` now run first, `backlightInit()` last, so
    the backlight only ever illuminates a screen this boot has already
    written to black. Three lines moved, no new state, no new control
    flow. The one behavior change in the failure path: if `tft->begin()`
    itself fails, `backlightInit()` is no longer called at all (previously
    it always ran first regardless) — harmless, since `displayReady`
    stays false either way and `splashLine()`/`playBootMark()` already
    no-op on that path, so nothing was ever visible there to lose.
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS**, RAM and
    flash **byte-identical** to v0.6.4 (50332/327680B, 969529/3342336B) —
    expected, since this is a pure call-order swap of two existing
    functions, not new code. `pio test -e native` **90/90** unaffected
    (`main.cpp`'s boot sequence isn't part of the host-native build, so
    this class of bug is inherently untestable off real hardware). **Not
    yet re-confirmed against a genuine warm reboot on the bench** — the
    report was one photo from the field, not a bench session with the
    fix applied; next session should specifically power-cycle and
    EN-reset the device a few times watching for whether the prior page
    is now fully invisible, not just shortened, since the exact
    reset-to-backlight-on timing on this specific board hasn't been
    measured.
  - **Version: v0.6.4 -> v0.6.5.** PATCH-level — a correctness fix to the
    existing boot sequence, no new build-order scope.
- **2026-08-25 (even later still) — v0.6.6: the boot mark's arcs were
  rotated 90° off — fixed and verified by simulation, not just re-read.**
  Same reboot photo used for the v0.6.5 investigation also showed the
  problem round 4/5 explicitly flagged as unverified: the icon's arcs
  drooped down off the pole tip like a wilted flag, instead of fanning
  right like the approved mockup.
  - **Root cause: the round 4/5 angle-convention comment was wrong, not
    just unverified.** It claimed `fillArc()` measures degrees from 12
    o'clock (0°), clockwise, and added a +90° constant to convert from
    the mockup canvas's 0°=3-o'clock-clockwise convention — stated as
    "confirmed against the vendored Arduino_GFX.cpp." That confirmation
    was a re-read of the source's scanline math, not an actual trace of
    what it computes. This session ported `writeFillArcHelper()` line for
    line to Python and rasterized `fillArc(cx, cy, r, r-2, θ, θ+1, ...)`
    for probe angles around the full circle to empirically map angle to
    on-screen direction. Result: `fillArc()` already uses 0° = 3 o'clock,
    clockwise — the *same* convention as canvas, not the 12-o'clock one
    the comment claimed. The +90° was therefore a real bug: it rotated
    the whole mark 90° clockwise, turning the intended right-facing
    signal fan into a downward droop. (Sanity check: with the old
    `45.3°`/`134.1°` values, `writeFillArcHelper`'s own quadrant-trim
    logic — `if (end<180 && start<180) y = 0` — collapses the scan range
    to `y >= cy` only, i.e. strictly the *bottom* half of the circle
    relative to the anchor. That alone should have been a tell in round
    4/5 that the arc could not be symmetric about the horizontal-right
    axis as intended.)
  - **The fix:** `MARK_ARC_START_DEG`/`MARK_ARC_END_DEG` (`main.cpp`) now
    hold the mockup's canvas angles directly — `-44.7f`/`44.1f` — with no
    conversion applied, since none is needed. Rasterized both the old and
    new values with the same ported scanline logic before shipping:
    the old angles produce a shape entirely below the anchor (dy range
    +5 to +21, matching the photo's droop); the new angles produce a
    shape entirely to the right of the anchor, symmetric top-to-bottom
    (dx range +5 to +21, dy range -15 to +15) — a nested right-facing
    bracket, matching the approved mockup. Rendered as a small preview
    bitmap from the same simulation and visually confirmed it reads as
    a right-facing signal/WiFi fan before committing to the change.
  - **Also specifically re-checked the "black box" artifact** this
    session's round 4/5 entry logged as reported-but-unreproducible (in
    a mockup screenshot, not on real hardware). Cropped and 4x-zoomed the
    same reboot photo's dark regions; a brightness+contrast-boosted pass
    first appeared to show a faint box, but a side-by-side *raw*
    (unenhanced) crop of the identical region showed nothing — the "box"
    was JPEG compression block structure made visible by the enhancement
    itself, not real panel content. **Still unreproduced on real
    hardware, now including this on-device photo, not just the mockup.**
    (The short diagonal mark near the photo's bottom-left corner is
    expected content, not an artifact: it's the diagonal-foot path's
    first segment, `MARK_PATH_X0/Y0` at `(6,130)` — round 4/5's own
    addition — rendering in isolation because the panel's bottom-left
    corner is far from the pole's main vertical run.)
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS**, RAM/flash
    **byte-identical** to v0.6.5 (50332/327680B, 969529/3342336B) —
    expected, a two-constant value change touches no code path shape.
    `pio test -e native` **90/90** unaffected (`main.cpp` isn't part of
    the host-native build). **Not yet re-confirmed on real hardware** —
    same standing caveat as every boot-mark round to date; the next bench
    pass should specifically confirm the arcs now read as a right-facing
    fan on the actual ST7789V2, not just in this session's simulation.
  - **Version: v0.6.5 -> v0.6.6.** PATCH-level — a correctness fix to
    already-shipped boot-mark geometry, no new build-order scope.
- **2026-08-25 (even later still) — v0.6.7: the signal-trace flourish was
  erasing the pole/diagonal-foot on every animation frame.** Operator
  screenshot of the corrected-arcs build showed a solid black gap
  partway down the pole, with only the diagonal foot's tail surviving
  below it — reported as also visible in last session's HTML mockup.
  Unlike the "black box" report chased down in the v0.6.6 entry above,
  this one was a clean render (not a compressed phone photo), so there
  was no enhancement artifact to rule out — it needed tracing through the
  actual draw calls instead.
  - **Root cause:** `drawSignalTraceFrame()` (`main.cpp`) clears a
    horizontal band before drawing each of the trace's 7 animation
    frames — `fillRect(0, TRACE_Y - TRACE_AMP - 2, TFT_PANEL_WIDTH,
    TRACE_AMP * 2 + 4, SPLASH_BG)`. That's the **full panel width**
    (x=0-240), not the trace's own drawing region (`TRACE_X0` to
    `TRACE_X1`, i.e. x=39-236). The diagonal-foot path segment
    (`MARK_PATH_X0/Y0` at (6,130) to `MARK_PATH_X1/Y1` at (14,108)) and
    the pole's own tail (the vertical run down to y=108) are drawn once,
    earlier in `playBootMark()`, and physically fall inside this exact
    y-band (`TRACE_Y=112 ± TRACE_AMP=9`, plus 2px padding — y≈101 to
    123) at x=6-14, well left of `TRACE_X0=39`. Every one of the 7 frames
    re-clearing x=0-240 wiped a bit more of that already-drawn artwork,
    and since nothing in `drawSignalTraceFrame()` ever redraws x<39,
    the final frame left that region flat black — a solid gap between
    the visible upper pole and the surviving tail of the foot (the part
    of the foot below y=123, outside the band). The round 4/5 comment
    on that line ("leaving the mark/wordmark above untouched") was true
    as far as it went — the arcs and wordmark are well above `TRACE_Y`
    — but never accounted for the L-path's own bottom segments dipping
    into the same y-range.
  - **The fix:** clip the band-clear to `[TRACE_X0, TRACE_X1)` — the
    exact x-range the trace pattern itself ever draws in —
    `fillRect(TRACE_X0, TRACE_Y - TRACE_AMP - 2, TRACE_X1 - TRACE_X0,
    TRACE_AMP * 2 + 4, SPLASH_BG)`. Nothing the trace owns lives outside
    that x-range, and nothing needs re-clearing there since the pole/foot
    are drawn exactly once, before the animation loop even starts — no
    reason to keep touching x<39 on every frame.
  - **Verification:** simulated the full sequence (path segments, all 3
    arcs at the corrected angles, all 7 trace frames) with the same
    ported-to-Python draw logic used for the v0.6.6 arc fix, faithfully
    replicating `drawLine`/`fillRect`/the arc rasterizer's real behavior
    rather than just reasoning about it — confirmed the pole and
    diagonal foot now render fully intact and continuous end to end, with
    the trace band correctly confined to its own region. `pio run -e
    cardputer-adv` **SUCCESS**, RAM/flash **byte-identical** to v0.6.6
    (50332/327680B, 969529/3342336B) — same single `fillRect()` call,
    narrower bounds, no new code. `pio test -e native` **90/90**
    unaffected (`main.cpp` isn't part of the host-native build). **Not
    yet re-confirmed on real hardware** — same standing caveat as every
    boot-mark round; next bench pass should confirm the full mark (path,
    arcs, and trace together) renders clean start to finish on the actual
    panel, not just in simulation.
  - **Version: v0.6.6 -> v0.6.7.** PATCH-level — a correctness fix to
    already-shipped boot-mark geometry, no new build-order scope.

- **2026-08-25 (even later still) — v0.6.8: boot-checklist hold delay, and
  the boot profile is now persisted instead of always defaulting to
  Meshtastic.** Two operator-reported/requested items from the same bench
  session, hardware-confirmed.
  - **Boot-checklist hold.** The panel went from "Radio: OK" straight to
    `uiTaskStart()`'s status pages with no pause, so the completed
    checklist was barely visible before the UI repainted over it.
    `main.cpp` now holds for a new `BOOT_CHECKLIST_HOLD_MS` (2500ms)
    right after the "Radio: OK" splash line, before `uiTaskStart()` takes
    the panel. A plain `delay()`, no new state or draw calls.
  - **Boot profile persistence.** `main.cpp` always started
    `radioTaskStart()` on `MissionProfile::MESHTASTIC`, even though the
    per-profile channel overrides (2026-08-24 entry above) were already
    being remembered across boots — an operator who switched to MeshCore
    via the menu and powered off would come back up listening for
    Meshtastic on every reboot, same class of surprise the channel-override
    bug was. New `src/profile_state.h`/`.cpp` mirrors
    `display_settings.h`/`.cpp`'s load/write pair exactly:
    `loadLastProfileFromSD()` runs at boot (after
    `loadProfileOverridesFromSD()` has already mounted the card) and
    resolves `bootProfile` before `radioTaskStart()` is called;
    `writeLastProfileToSD()` is called from `ui_actions.cpp` right after a
    menu-driven `SELECT_MESHTASTIC`/`SELECT_MESHCORE` switch. New file
    `/loratrace/profile.txt` (`active_profile=meshtastic`/`meshcore`),
    sibling to `config.txt`/`display.txt`. Only Meshtastic/MeshCore are
    covered — Reticulum/General Exploration have no live menu switch path
    yet, so there's nothing else to round-trip. Fails safe the same way
    every other SD-backed setting here does: missing card/file/bad value
    leaves the caller's profile untouched (defaults to Meshtastic on a
    truly first boot).
  - **Verification:** `pio run -e cardputer-adv` **SUCCESS** (RAM
    50332/327680B, flash 971001/3342336B, +1472B for the new module),
    `pio test -e native` **90/90** unaffected (neither change touches
    host-testable code). **Confirmed on real hardware by the operator**:
    the checklist hold is visible before the UI takes over, and a
    menu-driven profile switch survives a power cycle.
  - **Version: v0.6.7 -> v0.6.8.** PATCH-level — both are fixes to
    already-shipped boot behavior, no new build-order scope.

- **2026-08-25 (even later still) — Docs reorg: PROGRESS.md's decisions
  log extracted into this file, SECURITY.md added.** Operator flagged that
  PROGRESS.md's tail had become an unbounded rolling changelog (new dated
  entries kept getting appended after the "Next steps" section rather than
  staying under the "Decisions log" heading, growing past the point of the
  file staying a readable status doc). Split: every dated decisions-log
  entry (both the ones under the original heading and the ones that had
  drifted past "Next steps") moved here, in the same chronological order;
  PROGRESS.md now keeps only Current status, the Build-order checklist,
  Open questions, a short pointer to this file, and Next steps. No content
  was reworded or reordered in the move, only relocated. Also added
  SECURITY.md (supported versions, known attack surface — the WiFi AP's
  fixed default WPA2 password, plaintext SD-stored detection logs, no
  secure boot/flash encryption, no serial command console — data
  sensitivity notes, and a vulnerability-reporting path), since none
  existed despite the field-deployed WiFi AP + web UI already being real
  attack surface. Docs-only change; no firmware version bump (`src/
  version.h`'s convention tracks build-order phase/fixes, not doc
  reorganization).

- **2026-08-26 — Phase 7 inserted for device optimization; first memory
  instrumentation build completed.** The operator chose to make measured
  device headroom its own epic before adding either planned scan mode. The
  old Phase 7 `DISCOVERY_SWEEP` is now Phase 8 and old Phase 8
  `ENERGY_SWEEP` is now Phase 9; neither feature's RF scope changed.
  ROADMAP.md carries the priority order and exit criteria, while new
  `HARDWARE_TESTING.md` owns the repeatable boot/receive/WiFi/browser/
  combined-load/soak matrix so `PROGRESS.md` can remain the result record.
  - Added `memory_stats.h/.cpp`: internal 8-bit heap free/minimum/largest
    block and free/allocated block counts, plus stack high-water marks for
    radio, GPS, logger, UI, and WiFi tasks. Bounded `[mem]` serial snapshots
    bracket the indexed canvas, AP start/stop, and CSV downloads; nothing
    runs per packet.
  - Appended the new heap and task-stack fields to `session.csv` rather
    than inserting them, preserving every pre-Phase-7 column position.
    A zero task-stack field explicitly means the task had not registered at
    that sample; periodic rows after startup should contain all five.
  - This is observability, not an optimization claim. No stack was reduced,
    WiFi lifecycle changed, or canvas replaced before the hardware baseline.
    `pio test -e native` **91/91 passed** and the single-threaded
    Cardputer-Adv build **succeeded** at 50348/327680B static RAM and
    972209/3342336B flash. The first parallel build was killed by host
    memory pressure while compiling the display library; rerunning `-j 1`
    completed, confirming it was not a firmware source failure.
  - **Version remains v0.6.8.** Phase 7 maps to v0.7.x, but the repository's
    version rule advances only when the phase's real-device matrix and
    final soak pass, not when its instrumentation compiles.

- **2026-08-26 — Fixed the first measured Phase-7 WiFi lifecycle leak.**
  Real-device run0065 held a flat 233952B free heap for the 10-minute
  WiFi-off MeshCore baseline, then exposed a 716B/13-allocation loss on the
  second AP cycle. `startAp()` was registering the same five heap-backed
  `WebServer` route handlers (plus the not-found callback) on every start;
  `WebServer::stop()` closes the listener but retains those handlers for
  the server object's lifetime.
  `registerRoutes()` is now idempotent, preserving on-demand first-use
  allocation without duplicating handlers on later starts. The cold-to-warm
  one-time WiFi framework cost is tracked separately and is not labeled a
  leak. Host tests/build verify the fix compiles; the ten-cycle hardware
  recovery test remains the next bench gate rather than being claimed here.

- **2026-08-27 — Phase 7 closed as v0.7.0.** Run0099 established the cold
  boot/idle/receive/UI baseline; run0102 supplied 7,596 seconds of final-build
  MeshCore soak evidence with flat post-warm-up heap; and run0108 validated the
  CSV-download watchdog fix on hardware. P1 and P3 were explicitly closed as
  measured no-change decisions, and the single fixed 2.5KB transient result
  buffer was conditionally accepted for future Probe/Sweep work. The operator
  waived repeating the unaffected A-E matrix on the final download-fix build;
  that nominal-criterion deviation is recorded in PROGRESS.md rather than
  being presented as same-build evidence.
