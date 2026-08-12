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

## Proposed layout (not yet created)

```
src/
  main.cpp
  radio_task.cpp / .h        # mission-profile state machine, owns SX1262
  gps_task.cpp / .h          # NMEA parse, last-fix mutex
  logger_task.cpp / .h       # dequeue, GPS-stamp, batched SD writes
  ui_task.cpp / .h           # keyboard + display
  channel_plans.h            # per-profile RF param tables (see DESIGN.md §3)
  fingerprint.h              # post-hoc protocol classification (§6)
platformio.ini
DESIGN.md
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

Design complete (see DESIGN.md). No code written yet. Build order is
DESIGN.md §9 — start with phase 1 (RadioLib bring-up, hardcoded Meshtastic
RX, print to serial) before touching the task/queue architecture.

## Related context

[[meshmapper-pipeline]] already holds real-world MeshCore frequency
observations for this area — check it before trusting a scraped "US
default" number for the channel tables.
