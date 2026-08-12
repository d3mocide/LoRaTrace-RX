# LoRaTrace
Passive LoRa and sub-GHz field logging

RX-only LoRa/sub-GHz wardriving firmware for the M5Stack Cap LoRa-1262
(SX1262) riding on a Cardputer-Adv (ESP32-S3). GPS-tagged detection
logging across four mission profiles: Meshtastic, MeshCore, Reticulum, and
general LoRa/spectrum exploration.

- **`DESIGN.md`** — hardware, RF parameters, and architecture rationale.
  Read this before making architecture changes.
- **`ROADMAP.md`** — build-order phases, MVP-Beta scope, and an honest
  feasibility assessment against this hardware's real limits.
- **`PROGRESS.md`** — current build status, open questions, decisions log.
- **`BRAND.md`** — naming, tone, and on-device UI copy conventions.
- **`CLAUDE.md`** — project rules for AI-assisted development on this repo.

## Status

Phase 1 (RadioLib bring-up, hardcoded Meshtastic RX) scaffolded, not yet
run on hardware. See `PROGRESS.md` for the live checklist.

## Build

PlatformIO + Arduino framework:

```
pio run
pio run --target upload
pio device monitor
```
