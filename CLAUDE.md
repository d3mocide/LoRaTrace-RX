# CLAUDE.md — LoRaTrace

## What this is

RX-only LoRa wardriving firmware for a Cardputer-Adv (ESP32-S3) + M5Stack Cap
LoRa-1262 (SX1262). Four mission profiles: Meshtastic War Drive, MeshCore War
Drive, Reticulum War Drive, General LoRa Exploration. GPS-tags detections to
SD. Full rationale and RF parameters live in `DESIGN.md` — read it before
making architecture changes, don't re-derive decisions already made there.

## Hardware assumptions

- ESP32-S3FN8: **no PSRAM.** Never assume it's available. Keep heap
  allocations small and static where possible; SD is the datastore, not RAM.
- SX1262 lives on its own SPI host, isolated from the display bus.
- Antenna path requires PI4IOE5V6408 IO-expander P0 driven high once at
  boot (I2C, G8/G9) — radio is silent without this regardless of code
  correctness. Init order matters.
- Radio task is pinned to Core 1 and must never block on SD or display I/O.
  Everything crosses to Core 0 via a FreeRTOS queue.

## Build system

Assuming PlatformIO + Arduino framework + RadioLib for the SX126x driver —
flag if you'd rather use ESP-IDF directly or a different driver; nothing
here depends on that choice except the specific API calls.

## Proposed layout

`[x]` created, `[ ]` proposed but deferred until its build-order phase
(DESIGN.md §9) — see ROADMAP.md/PROGRESS.md for why the task/queue files
aren't scaffolded yet.

```
src/
  [x] main.cpp                   # phase 2: orchestrator — boot order, then starts tasks
  [x] radio_task.cpp / .h        # HOME_LISTEN, owns SX1262, Core 1, never blocks
  [x] gps_task.cpp / .h          # NMEA parse, last-fix mutex, Core 0
  [x] logger_task.cpp / .h       # dequeue, GPS-stamp, batched SD writes, Core 0
  [x] ui_task.cpp / .h           # keyboard + display; arrived phase 2, not phase 6 as originally proposed — operator asked for field-readable status before a multi-hour run without a tethered laptop (see PROGRESS.md decisions log)
  [x] keyboard.h                 # TCA8418 raw-event -> KeyAction decode for 4 keys, pure/host-testable (phase 5; not in original proposal, see PROGRESS.md decisions log)
  [x] wifi_task.cpp / .h         # WiFi AP + web UI, off until toggled (phase 3; not in original proposal, see PROGRESS.md decisions log)
  [x] web_assets.h               # embedded single-page web UI (phase 3)
  [x] channel_plans.h            # per-profile RF param tables (see DESIGN.md §3)
  [x] board_pins.h               # pin map + IO-expander register constants (not in original proposal, see PROGRESS.md decisions log)
  [x] version.h                  # FIRMWARE_VERSION, single source for boot banner + release tags
  [x] config.h / .cpp            # boot-time SD channel-config override + runtime write-back for wifi_task's settings page (not in original proposal, see PROGRESS.md decisions log)
  [ ] fingerprint.h              # post-hoc protocol classification (§6, phase 6+)
  --- added during phase 2, not in the original proposal ---
  [x] detection.h                # the ~36B queue record + Meshtastic header parse + §8 CSV
  [x] nmea.h                     # NMEA primitives (field split, checksum, coord conversion)
  [x] gps_parse.h                # GpsFix + gpsApplySentence(), pure so it's host-testable
  [x] spi_bus.h / .cpp           # mutex + SPIClass for the bus SD and the SX1262 SHARE
  [x] serial_lock.h / .cpp       # mutex guarding cross-core Serial writes (not in original proposal, see PROGRESS.md decisions log)
  [x] io_expander.h / .cpp       # PI4IOE5V6408 P0: antenna switch AND GPS power
  [x] gps_probe.cpp              # standalone GPS bring-up sketch ([env:gps-probe])
test/
  [x] test_channel_plans/        # host-native unit tests, pio test -e native
  [x] test_detection/            # queue-record + CSV, fixtures are REAL captured packets
  [x] test_gps_parse/            # NMEA -> fix, mostly about REFUSING a bad position
  [x] test_keyboard/             # raw TCA8418 event byte -> KeyAction, allowlist-only
.github/workflows/
  [x] build.yml                  # pio run + pio test on every push/PR + rolling dev-latest release
  [x] release.yml                # vX.Y.Z tag -> draft GitHub Release with Launcher-ready .bin
sd-template/loratrace/
  [x] config.txt                 # copy-to-SD-card example for the channel config override
[x] platformio.ini
[x] DESIGN.md
[x] ROADMAP.md
[x] PROGRESS.md
```

## House rules

- **RX-only.** No transmit path beyond what antenna-switch init requires.
  Don't add TX/injection features without an explicit ask.
- **Don't hardcode sync-word values** without verifying against upstream
  firmware source first. Both are now **resolved and cited** in
  `channel_plans.h`: Meshtastic **0x2B** (meshtastic/firmware
  `src/mesh/RadioLibInterface.h`), MeshCore **0x12** (meshcore-dev/MeshCore
  `src/helpers/radiolib/CustomSX1262.h`). MeshCore's equals RadioLib's own
  default — keep `SYNC_WORD_MESHCORE` and `SYNC_WORD_RADIOLIB_DEFAULT` as
  separate constants anyway; they mean different things and only coincide
  today. Note the sync word is an RX *filter*: a wrong value means hearing
  nothing from the target protocol while still hearing unrelated traffic
  that matches — that exact bug cost several bench sessions (PROGRESS.md
  2026-08-23).
- **Don't assume MeshCore's encryption mirrors Meshtastic's default-PSK
  model** — it doesn't necessarily; MeshCore's own docs warn against this.
- No large heap buffers. Detection struct is small (~40B); flush to SD
  often rather than accumulating.
- **Bump `src/version.h` when a phase lands.** `MAJOR.MINOR` tracks the
  build-order phase (ROADMAP.md Versioning: v0.1.x = phase 1, v0.2.x =
  phase 2, ...), `PATCH` for fixes adding no phase scope. It is bumped by
  hand on purpose — it asserts "this phase is reached", and a number that
  auto-increments every build asserts nothing. `release.yml` now fails a
  tag whose version doesn't match the header, so a mismatch can't ship.
  Build *provenance* is the automated half: `scripts/build_rev.py` injects
  `FIRMWARE_BUILD_REV` (git short SHA, `-dirty` when the tree is modified)
  into every build, because a dozen `dev-latest` binaries otherwise share
  one version string and a hardware bug report can't name which one it hit.

## Status

**Phase 1 complete and hardware-verified** (2026-08-23): radio, antenna
path, SD config override, display, and protocol-correct Meshtastic RX all
confirmed on real hardware. Heap stable under load (~338KB, no leak).

**Phase 2 complete and hardware-verified** (2026-08-23): the task/queue
architecture, GPS task, and batched SD logger held up over a genuine
multi-hour unattended run (run0007, 2.5h) — every exit-criterion counter
(`crc_err`/`queue_drop`/`bus_miss`/`row_drop`) stayed at 0 the whole time,
heap flat with no leak, continuous 3D GPS fix throughout. See PROGRESS.md
for the numbers.

**Phase 3 (WiFi AP + web UI) built and hardware-verified** (2026-08-23,
reconfirmed 2026-08-24 under live traffic): pulled forward ahead of MeshCore
at the user's request — see ROADMAP.md for why. `wifi_task` is off by
default and only starts the AP on an operator gesture, specifically so it
never runs during an actual drive unless asked for. The go/no-go this phase
was gated behind — `ESP.getFreeHeap()` before/after `WiFi.softAP()` with the
full task set running, radio counters staying at 0 with the AP active — is
answered: ~55-56KB heap cost for the AP (measured twice, a day apart, same
number both times), `crc_err`/`queue_drop`/`bus_miss`/`row_drop` all stayed
at 0 with the AP active including a 2026-08-24 session with 800+ real
detections logged while it ran. See PROGRESS.md for both measurements.

**2026-08-24 addendum:** the channel-config settings page originally
shipped here had a real bug, not just a missing feature — one shared
preset applied to whichever profile was active at Save time, so saving
while on MeshCore silently corrupted what the firmware would apply as a
*Meshtastic* override on the next boot, and a profile switch back to a
profile always reverted to its hardcoded default regardless of what had
been configured for it. Fixed the same day with per-profile overrides
(`ProfileOverrides`, `channel_plans.h`) — see PROGRESS.md's Decisions log
for the full design. Still not hardware-verified, v0.5.1.

**Phase 4 (MeshCore profile) built and fully hardware-verified**
(2026-08-24): the MeshCore US-narrow table (910.525MHz/SF7/BW62.5/CR5)
already existed in `channel_plans.h` from earlier sync-word research — what
Phase 4 actually adds is DESIGN.md §5's keyboard-gated, mutually-exclusive
runtime switch between it and Meshtastic, deliberately deferred from Phase
3 (see PROGRESS.md's 2026-08-23 decisions log) so it could be built against
a real second table instead of a stub. `radio_task.cpp` gained a depth-1
`xQueueOverwrite` mailbox so `ui_task`'s keyboard poll can request a live
retune without the radio task ever blocking on it; the original ~3s-hold
trigger was later replaced by Phase 5's menu. A MeshCore detection logs
RSSI/SF/BW/timing and the profile tag but not `node_id`/`packet_id`:
MeshCore's header layout isn't reverse-engineered and its encryption/PSK
model is still open (§7), so `radio_task.cpp` never runs Meshtastic's
header parser against it. Verified against the host-native test suite (73
tests) and, as of 2026-08-24, on real hardware: live MeshCore RX at
910.525MHz/SF7/BW62.5/CR5 with plausible RSSI/SNR (a dozen+ detections,
-58 to -64dBm), and a mid-run Meshtastic<->MeshCore switch that left
`crc_err`/`queue_drop`/`bus_miss`/`row_drop` unaffected in both directions
— with a genuine Meshtastic node and relay pair heard on one side and clean
MeshCore detections on the other, in the same run. See PROGRESS.md's
Decisions log for the full readout.

**Phase 5 (on-device menu UI) built and fully hardware-verified**
(2026-08-24): pulled forward ahead of `DISCOVERY_SWEEP`/`ENERGY_SWEEP` at
the user's request — same restructuring precedent as WiFi's Phase-3
pull-forward, see ROADMAP.md. Replaces Phase 3/4's timed hold-gestures with
real keyboard-driven navigation: `keyboard.h` decodes eleven specific
TCA8418 keys — `,`/`.` move (aliased by `;`/`/`, the keyboard's own printed
Fn-arrow diamond, added after a first bench pass), Enter acts on the
highlighted menu row, the backtick/ESC key opens *and* closes the menu
(swapped in for Backspace as BACK the same bench session that found it;
gained the menu-*opening* job in a second bench-session UX fix — see
below), `1`-`5` jump straight to a numbered carousel page — no Fn chord
(the ESC/arrow additions deliberately bind the *plain* key rather than the
upstream Fn-combo, to keep this rule rather than adopt it), sourced against
three independent references since the Cardputer-ADV has no dedicated arrow
keys, see DESIGN.md §10. `ui_task` gained a two-item menu (profile switch,
WiFi toggle — both reusing existing Phase 3/4 actions unchanged) plus a new
read-only CHANNEL status page. Deliberately narrow scope, decided with the
user up front: toggles only, no on-device numeric editing of channel params
(that stays on the web UI/`config.txt`). Verified against the host-native
test suite (73 tests).

**Full hardware bench pass, 2026-08-24, all items closed.** First pass
confirmed Comma, Period, and all five digit keys working as designed, and
surfaced the ESC/arrow-alias changes above (Backspace-as-BACK felt wrong in
hand; the operator's own attempt to use the printed Fn-arrow diamond
exposed `;`/`/` weren't wired yet). A second pass the same day confirmed
ESC/backtick and Semicolon/Slash on real hardware, then surfaced one more
UX finding acted on immediately: opening the menu with Enter "feels kind of
weird." Fixed by moving that job onto ESC/backtick instead (closes the menu
as before, now also opens it from the carousel; Enter narrowed to just
firing the highlighted row, no-op in the carousel) — re-flashed and
re-confirmed on hardware same session. Menu actions (profile switch, WiFi
toggle) and the CHANNEL page's live-switch reflection are confirmed
working too. See PROGRESS.md's Decisions log for the full session,
including a live-traffic reconfirmation of Phase 3's WiFi heap/counter
numbers that fell out of the same test.

**Verbose serial debug mode** (`logger_task.cpp`, menu's third `Debug` row,
added and hardware-verified 2026-08-24): prints each detection's full
CSV-shaped row to serial as it's dequeued, off by default. Not a planned
phase item — added because Phase 4's live-RX bench check needed to see real
RSSI/SNR and nothing else surfaces per-packet detail without an SD pull or
the WiFi AP. Lives in `logger_task.cpp` rather than `radio_task.cpp` on
purpose: it prints on Core 0 after the detection has already crossed the
queue, so the radio task on Core 1 can never block on it. Shipped with a
real bug caught the same session — its first version used multiple `Serial`
calls per line and came out torn on real hardware by `main.cpp`'s Core-1
`[status]` line landing mid-sequence, the same failure mode that comment
already documents. Fixed by collapsing each line to one buffered
`Serial.write()`. Output-only by design: a serial *command* console was
floated in the same conversation and deliberately deferred — real added
surface (a parser, and what's safe to expose) on an RX-only tool, worth
deciding on its own rather than folding into a quick debug-visibility ask.

**Serial console now has a real mutex** (`serial_lock.h`/`.cpp`, added
2026-08-24, mirrors `spi_bus.h`'s `SpiBusLock` exactly): every `Serial`
print site in the firmware takes it, except `applyConfigLine()`/
`loadProfileOverridesFromSD()`/`writeFullConfig()` in `config.cpp` (all
three are only ever called once from `setup()` before any task exists —
provably single-threaded) and `radio_task.cpp` (doesn't print at all, and
must never block on non-radio I/O — keeping it that way was a design
constraint on this fix). The 2026-08-23 "one buffer, one `Serial` call"
fix turned out to be necessary but not sufficient: a 2026-08-24 hardware
session got a real WiFi client to join the AP and save Settings — the
first time that single-buffer fix was tested under real concurrent load —
and it teared anyway (`[wifi] AP started: L`, a `[config]` write missing
everything before `BW62.5`). The mutex fixed it; re-tested with the same
real-load scenario and every message came through intact, including the
exact `[config]` line that broke before. **One unexplained residual
anomaly** (a different `[config]` line lost its prefix despite the lock,
while everything around it was clean) reads as a lower-level USB-CDC
single-write truncation, not cross-task interleaving — logged as a watch
item in PROGRESS.md rather than chased further without a logic analyzer.

Three hard-won rules from Phases 1–2, worth not relearning:
- **The IO expander's P0 powers the GPS as well as switching the RF antenna
  path.** A "dead" GPS or a silent radio is often just this.
- **A wrong sync word is silent, not loud** — the radio simply never
  interrupts, while still hearing unrelated traffic that matches.
- **A same-node-id detection pair with wildly different RSSI seconds apart
  is very likely a genuine mesh relay, not a logging bug** — `packet_id`/
  `hop_limit`/`hop_start`/`relay_node` in `detections.csv` (added after
  run0007) prove it either way; don't assume duplicate-detection without
  checking those first.

Build order is DESIGN.md §9. See PROGRESS.md for the live build checklist
and ROADMAP.md for phase-by-phase scope.

## Related context

[[meshmapper-pipeline]] already holds real-world MeshCore frequency
observations for this area — check it before trusting a scraped "US
default" number for the channel tables.
