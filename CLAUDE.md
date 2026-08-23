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
  [ ] ui_task.cpp / .h           # keyboard + display (phase 6)
  [x] channel_plans.h            # per-profile RF param tables (see DESIGN.md §3)
  [x] board_pins.h               # pin map + IO-expander register constants (not in original proposal, see PROGRESS.md decisions log)
  [x] version.h                  # FIRMWARE_VERSION, single source for boot banner + release tags
  [x] config.h / .cpp            # boot-time SD channel-config override (not in original proposal, see PROGRESS.md decisions log)
  [ ] fingerprint.h              # post-hoc protocol classification (§6, phase 4+)
  --- added during phase 2, not in the original proposal ---
  [x] detection.h                # the ~36B queue record + Meshtastic header parse + §8 CSV
  [x] nmea.h                     # NMEA primitives (field split, checksum, coord conversion)
  [x] gps_parse.h                # GpsFix + gpsApplySentence(), pure so it's host-testable
  [x] spi_bus.h / .cpp           # mutex + SPIClass for the bus SD and the SX1262 SHARE
  [x] io_expander.h / .cpp       # PI4IOE5V6408 P0: antenna switch AND GPS power
  [x] gps_probe.cpp              # standalone GPS bring-up sketch ([env:gps-probe])
test/
  [x] test_channel_plans/        # host-native unit tests, pio test -e native
  [x] test_detection/            # queue-record + CSV, fixtures are REAL captured packets
  [x] test_gps_parse/            # NMEA -> fix, mostly about REFUSING a bad position
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

## Status

**Phase 1 complete and hardware-verified** (2026-08-23): radio, antenna
path, SD config override, display, and protocol-correct Meshtastic RX all
confirmed on real hardware. Heap stable under load (~338KB, no leak).

**Phase 2 written, not yet hardware-verified**: the task/queue architecture,
GPS task, and batched SD logger exist and build, and all pure logic is
covered by host tests — but no multi-hour unattended run has happened yet,
which is the actual exit criterion (ROADMAP.md). GPS in particular has
never produced a fix on this board; use `[env:gps-probe]` to bring it up
standalone before debugging it through the task.

Two hard-won rules from Phase 1, worth not relearning:
- **The IO expander's P0 powers the GPS as well as switching the RF antenna
  path.** A "dead" GPS or a silent radio is often just this.
- **A wrong sync word is silent, not loud** — the radio simply never
  interrupts, while still hearing unrelated traffic that matches.

Build order is DESIGN.md §9. See PROGRESS.md for the live build checklist
and ROADMAP.md for phase-by-phase scope.

## Related context

[[meshmapper-pipeline]] already holds real-world MeshCore frequency
observations for this area — check it before trusting a scraped "US
default" number for the channel tables.
