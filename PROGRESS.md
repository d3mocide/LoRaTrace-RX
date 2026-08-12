# LoRaTrace RX — Progress Tracking

Living document. Update this alongside code changes — it's the "what's
actually true right now" source, `ROADMAP.md` is the "what's the plan"
source, `DESIGN.md` is the "why" source. Don't let them drift.

## Current status (2026-08-12)

Phase 0 (project scaffold) complete. Phase 1 (RadioLib bring-up) code
**confirmed to build cleanly** (`pio run`, `esp32-s3-devkitc-1`,
RadioLib 7.7.1: 312KB flash / 20.3KB static RAM) but **not yet run on real
hardware** — this environment has no board attached. A clean build rules
out compile-time API mismatches; it says nothing about whether the SPI/I2C
wiring, register values, or radio behavior are actually correct. Treat
runtime behavior in `src/` as reasoned-through-but-unverified until
someone flashes it and reports back.

## Build-order checklist

Mirrors `ROADMAP.md` phases / `DESIGN.md` §9.

- [x] **Phase 0** — platformio.ini, pin map, RF tables, phase-1 code, docs
- [ ] **Phase 1** — RadioLib bring-up (builds clean, hardware-untested)
  - [x] Confirm `pio run` builds successfully (312KB flash / 20.3KB RAM,
        `esp32-s3-devkitc-1`, RadioLib 7.7.1)
  - [ ] Flash to real Cardputer-Adv + Cap LoRa-1262 and confirm it runs
  - [ ] Confirm `esp32-s3-devkitc-1` board def actually matches this
        board's flash/pin config (no dedicated PlatformIO board def found)
  - [ ] Confirm PI4IOE5V6408 I2C address (0x43) and register map
        (0x03/0x05/0x07) against real hardware — sourced from a
        third-party driver, not the primary Diodes datasheet directly
  - [ ] Confirm FSPI is actually free (not claimed by the display bus)
  - [ ] Confirm RadioLib 7.7.x `SX1262::begin()` / `setDio1Action()`
        signatures match what's in `main.cpp` (pinned by `^7.7.1`, minor
        version drift possible)
  - [ ] Bench-verify RX against a known live Meshtastic LongFast (US)
        transmitter — RSSI/SNR should be plausible, not just non-crashing
- [ ] **Phase 2** — task/queue architecture, GPS, SD, Logger (MVP-Beta)
- [ ] **Phase 3** — MeshCore profile
- [ ] **Phase 4** — `DISCOVERY_SWEEP`
- [ ] **Phase 5** — `ENERGY_SWEEP`
- [ ] **Phase 6** — UI polish, WiFi upload go/no-go

## Open questions (from DESIGN.md §7 — verify before / during build)

- [ ] Meshtastic's exact SX126x sync-word register value (sources
      disagree — pull from Meshtastic firmware source or RadioLib's
      Meshtastic-compat example)
- [ ] MeshCore's encryption/PSK scheme (don't assume it mirrors
      Meshtastic's default-channel PSK model)
- [ ] microSD bus on this board revision — SPI or SDMMC, shared host with
      display or not
- [ ] CAD `symNum` tuning — needs bench testing against Semtech AN1200.48
- [ ] 868–923MHz front-end rolloff near 923MHz — empirical RSSI floor
      sweep once hardware is in hand
- [ ] Exact per-slot frequency spacing for Meshtastic's 104 US slots —
      pull the real table from firmware source, don't infer from BW alone
- [ ] Real `ESP.getFreeHeap()` under load — every "no PSRAM" risk call in
      ROADMAP.md is provisional until this number exists

## Open questions — Launcher distribution

- [ ] Exact button combo for the manual-restart-into-Launcher path.
      Confirmed to exist (2026-08-12), specific keys not yet recorded —
      note it in this file once known, and consider printing it on the
      boot banner/UI so it's not tribal knowledge.
- [ ] How much flash Launcher's own footprint (bootloader + its app
      partition + any data/SPIFFS it keeps) leaves free for user-installed
      apps on an 8MB device, especially with multiple firmwares installed
      side by side. No documented number found — see ROADMAP.md
      Distribution section for the size budget reasoning used in the
      meantime.

## Decisions log

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

## Next steps

1. Get this scaffold onto real hardware; resolve the Phase 1 checklist
   above.
2. Once radio RX is confirmed live, empirically resolve the microSD bus
   question — it's the one Phase 2 blocker that can't be reasoned through
   from docs alone.
3. Start Phase 2 (task/queue architecture) only after Phase 1 is bench-
   confirmed, per CLAUDE.md's explicit build-order instruction.
