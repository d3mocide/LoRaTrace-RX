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
  [x] main.cpp                   # phase 1: direct bring-up, not yet a task
  [ ] radio_task.cpp / .h        # mission-profile state machine, owns SX1262 (phase 2)
  [ ] gps_task.cpp / .h          # NMEA parse, last-fix mutex (phase 2)
  [ ] logger_task.cpp / .h       # dequeue, GPS-stamp, batched SD writes (phase 2)
  [ ] ui_task.cpp / .h           # keyboard + display (phase 6)
  [x] channel_plans.h            # per-profile RF param tables (see DESIGN.md §3)
  [x] board_pins.h               # pin map + IO-expander register constants (not in original proposal, see PROGRESS.md decisions log)
  [x] version.h                  # FIRMWARE_VERSION, single source for boot banner + release tags
  [ ] fingerprint.h              # post-hoc protocol classification (§6, phase 4+)
test/
  [x] test_channel_plans/        # host-native unit tests, pio test -e native
.github/workflows/
  [x] build.yml                  # pio run + pio test on every push/PR
  [x] release.yml                # vX.Y.Z tag -> draft GitHub Release with Launcher-ready .bin
[x] platformio.ini
[x] DESIGN.md
[x] ROADMAP.md
[x] PROGRESS.md
```

## House rules

- **RX-only.** No transmit path beyond what antenna-switch init requires.
  Don't add TX/injection features without an explicit ask.
- **Don't hardcode sync-word values** for Meshtastic/MeshCore without
  verifying against upstream firmware source first — DESIGN.md §7 flags
  this as unresolved, sources disagree.
- **Don't assume MeshCore's encryption mirrors Meshtastic's default-PSK
  model** — it doesn't necessarily; MeshCore's own docs warn against this.
- No large heap buffers. Detection struct is small (~40B); flush to SD
  often rather than accumulating.

## Status

Design complete (see DESIGN.md). Project scaffold + Phase 1 bring-up code
exist (`platformio.ini`, `src/`) but are not yet verified on real
hardware. Build order is DESIGN.md §9 — start with phase 1 (RadioLib
bring-up, hardcoded Meshtastic RX, print to serial) before touching the
task/queue architecture. See PROGRESS.md for the live build checklist and
ROADMAP.md for phase-by-phase scope.

## Related context

[[meshmapper-pipeline]] already holds real-world MeshCore frequency
observations for this area — check it before trusting a scraped "US
default" number for the channel tables.
