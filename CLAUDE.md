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
  [x] ui_task.cpp / .h           # lifecycle + input decode + main loop; arrived phase 2, not phase 6 as originally proposed — operator asked for field-readable status before a multi-hour run without a tethered laptop (see PROGRESS.md decisions log)
  [x] ui_pages.cpp               # status-page + menu/toast drawing, split out of ui_task.cpp (2026-08-25 cleanup pass, not a phase item — see PROGRESS.md decisions log)
  [x] ui_actions.cpp             # menu-action business logic (radio/wifi/backlight/SD calls a fired menu row makes), split out of ui_task.cpp same pass
  [x] ui_task_shared.h           # private, non-public state/helpers shared only across the three files above — not part of ui_task.h's own two-function API
  [x] keyboard.h                 # TCA8418 raw-event -> KeyAction decode for 4 keys, pure/host-testable (phase 5; not in original proposal, see PROGRESS.md decisions log)
  [x] ui_menu.h                  # grouped root/group menu state machine, pure/host-testable (phase 6; not in original proposal, see PROGRESS.md decisions log)
  [x] ui_labels.h                # BRAND.md on-device label lookups, pure/host-testable (phase 6; not in original proposal, see PROGRESS.md decisions log)
  [x] wifi_task.cpp / .h         # WiFi AP + web UI, off until toggled (phase 3; not in original proposal, see PROGRESS.md decisions log)
  [x] web_assets.h               # embedded single-page web UI (phase 3)
  [x] channel_plans.h            # per-profile RF param tables (see DESIGN.md §3)
  [x] board_pins.h               # pin map + IO-expander register constants (not in original proposal, see PROGRESS.md decisions log)
  [x] version.h                  # FIRMWARE_VERSION, single source for boot banner + release tags
  [x] config.h / .cpp            # boot-time SD channel-config override + runtime write-back for wifi_task's settings page (not in original proposal, see PROGRESS.md decisions log)
  [ ] fingerprint.h              # post-hoc protocol classification (§6, phase 7+)
  --- added during phase 2, not in the original proposal ---
  [x] detection.h                # the ~36B queue record + Meshtastic header parse + §8 CSV
  [x] nmea.h                     # NMEA primitives (field split, checksum, coord conversion)
  [x] gps_parse.h                # GpsFix + gpsApplySentence(), pure so it's host-testable
  [x] spi_bus.h / .cpp           # mutex + SPIClass for the bus SD and the SX1262 SHARE
  [x] serial_lock.h / .cpp       # mutex guarding cross-core Serial writes (not in original proposal, see PROGRESS.md decisions log)
  [x] io_expander.h / .cpp       # PI4IOE5V6408 P0: antenna switch AND GPS power
  [x] gps_probe.cpp              # standalone GPS bring-up sketch ([env:gps-probe])
  [x] battery.h / .cpp           # voltage + charge % via GPIO10/ADC1, M5Unified's board_M5CardputerADV constants (phase 2)
  [x] run_log.h                  # one wardrive = one /loratrace/runNNNN/ directory; run numbering (phase 2)
  [x] session_log.h              # session.csv periodic health record — Phase 2 exit-criterion evidence (phase 2)
  --- added during phase 6, not in the original proposal ---
  [x] backlight.h / .cpp         # ST7789V2 backlight PWM (LEDC), M5GFX's non-linear duty curve for this exact pin
  [x] display_settings.h / .cpp  # brightness + idle-dim timeout, persisted to /loratrace/display.txt, separate from config.h's channel-override scope
test/
  [x] test_channel_plans/        # host-native unit tests, pio test -e native
  [x] test_detection/            # queue-record + CSV, fixtures are REAL captured packets
  [x] test_gps_parse/            # NMEA -> fix, mostly about REFUSING a bad position
  [x] test_keyboard/             # raw TCA8418 event byte -> KeyAction, allowlist-only
  [x] test_battery/              # voltage/percent conversion, pure logic
  [x] test_run_log/              # run-directory numbering
  [x] test_session_log/          # session.csv row formatting
  [x] test_ui_labels/            # ui_labels.h's uiProfileLabel() lookups (phase 6)
  [x] test_ui_menu/              # ui_menu.h's MenuState, recursive/depth-bounded (phase 6)
.github/workflows/
  [x] build.yml                  # pio run + pio test on every push/PR + rolling dev-latest release
  [x] release.yml                # vX.Y.Z tag -> draft GitHub Release with Launcher-ready .bin
sd-template/loratrace/
  [x] config.txt                 # copy-to-SD-card example for the channel config override
[x] platformio.ini
[x] DESIGN.md
[x] ROADMAP.md
[x] PROGRESS.md
[x] CHANGELOG.md
[x] SECURITY.md
[x] AGENTS.md
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
- **New operator-facing behavior gets an on-device menu toggle**, not just
  a web-UI-only setting or a silent default. Established 2026-08-25 after
  Phase 5's menu grew a third item (verbose debug) the same day it
  shipped, with no framework change to absorb it — see PROGRESS.md's
  Decisions log and ROADMAP.md's Phase 6 (UI architecture redesign).
  Doesn't apply to one-shot boot-time config (channel overrides,
  `config.txt`) — this is about anything that changes runtime behavior
  while the device is already running.
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
- **Comments explain "why," briefly — not the full investigation.** State
  the fact/value/gotcha and, if needed, a one-line reason or citation
  (source repo/file, date, measured number). Don't retell an entire
  debugging session, restate what the next line already shows, or repeat
  a story that already lives in DESIGN.md/CHANGELOG.md — cite it instead
  (`see DESIGN.md §3`), don't re-paste it. Established 2026-08-25 after
  `board_pins.h`/`channel_plans.h`/`main.cpp` were found running
  62-72% comment lines, most of it narrative history duplicating
  CHANGELOG.md — trimmed that session; don't reproduce it going forward.
  Test before writing one: if you deleted this comment, could a reader get
  the same fact by checking DESIGN.md/CHANGELOG.md instead of re-deriving
  it from scratch? If yes, that's a pointer, not a paragraph.

## Status

**v0.6.8** (current, `src/version.h`). Phases 0-6 complete and
hardware-verified: radio bring-up (Phase 1), the task/queue architecture +
GPS + SD logging that makes up MVP-Beta (Phase 2), the WiFi AP + web
command center (Phase 3), the MeshCore profile and live profile switch
(Phase 4), the on-device menu UI (Phase 5), and the UI architecture
redesign — grouped menu, toast layer, canvas-based rendering to fix
real-hardware flicker/tearing, animated boot-mark splash, real PWM
backlight control, and persistence for display settings and last-active
profile (Phase 6). Phase 7 (`DISCOVERY_SWEEP`) and Phase 8
(`ENERGY_SWEEP`) are not started.

**Don't read PROGRESS.md or CHANGELOG.md end-to-end by default** — they're
reference/history, not required context for every task. For what's true
*right now*, see PROGRESS.md's Current status section and Build-order
checklist. For why something is the way it is, or the full story behind a
specific fix/version, grep CHANGELOG.md for the date/version/topic you
need rather than reading it front to back.

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
and ROADMAP.md for phase-by-phase scope. The session-by-session decisions
log itself (what every "see PROGRESS.md's Decisions log" reference above
points to) moved to CHANGELOG.md on 2026-08-25 — PROGRESS.md now only
holds current status, the checklist, open questions, and next steps.

## Related context

[[meshmapper-pipeline]] already holds real-world MeshCore frequency
observations for this area — check it before trusting a scraped "US
default" number for the channel tables.

