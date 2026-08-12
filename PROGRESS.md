# LoRaTrace RX — Progress Tracking

Living document. Update this alongside code changes — it's the "what's
actually true right now" source, `ROADMAP.md` is the "what's the plan"
source, `DESIGN.md` is the "why" source. Don't let them drift.

## Current status (2026-08-12)

Phase 0 (project scaffold) complete. Phase 1 (RadioLib bring-up) code
written but **not yet run on real hardware** — this environment has no
board attached. Treat everything in `src/` as reasoned-through-but-
unverified until someone flashes it and reports back.

## Build-order checklist

Mirrors `ROADMAP.md` phases / `DESIGN.md` §9.

- [x] **Phase 0** — platformio.ini, pin map, RF tables, phase-1 code, docs
- [ ] **Phase 1** — RadioLib bring-up (code exists, hardware-untested)
  - [ ] Flash to real Cardputer-Adv + Cap LoRa-1262 and confirm it builds
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

## Next steps

1. Get this scaffold onto real hardware; resolve the Phase 1 checklist
   above.
2. Once radio RX is confirmed live, empirically resolve the microSD bus
   question — it's the one Phase 2 blocker that can't be reasoned through
   from docs alone.
3. Start Phase 2 (task/queue architecture) only after Phase 1 is bench-
   confirmed, per CLAUDE.md's explicit build-order instruction.
