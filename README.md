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

Phase 1 (RadioLib bring-up, Meshtastic RX with optional SD channel
override) scaffolded, not yet run on hardware. See `PROGRESS.md` for the
live checklist.

## Build

PlatformIO + Arduino framework:

```
pio run -e cardputer-adv
pio run -e cardputer-adv --target upload
pio device monitor
```

Unit tests (host-native, no board needed):

```
pio test -e native
```

## Install without flashing (M5Launcher)

If your Cardputer-Adv already runs
[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher), you don't
need to touch USB flashing at all:

1. Download the latest build:
   `https://github.com/d3mocide/LoRaTrace-RX/releases/download/dev-latest/LoRaTraceRX-dev.bin`
   (rebuilt automatically from `main` on every merge — check that release's
   notes for the commit it came from). Tagged, more-stable versions are on
   the [Releases page](https://github.com/d3mocide/LoRaTrace-RX/releases).
2. Copy the `.bin` onto a FAT32 SD card.
3. In Launcher: SD → select the file → Install.
4. To get back to Launcher afterward: reset the device and press any key
   during Launcher's own ~5s boot window (it prints "Press the button to
   enter the Launcher!" over serial while waiting) — miss it and Launcher
   auto-boots straight back into whatever ran last. To skip that timing
   window entirely, enable Launcher's own Settings → "Boot to Launcher"
   toggle; it then always stops at its menu on reset until you turn it
   back off. Not a software hook this firmware implements — see
   `PROGRESS.md` for how this was confirmed against Launcher's own source.

Serial output still works normally over USB while running under Launcher
— `pio device monitor` to watch the boot banner and any `[RX]`/`[config]`
lines.

## Configuration

By default LoRaTrace RX locks to Meshtastic LongFast (US):
906.875MHz / SF11 / BW250kHz / CR4:8. The first time it boots with an SD
card that doesn't already have one, it creates `/loratrace/config.txt` on
the card pre-filled with those defaults — just edit that file in place for
a non-default regional preset (e.g. MeshOregon) and reboot. See the file's
own comments for the format. (`sd-template/loratrace/` still exists if you
want to prepare a card offline before ever booting the device with it.) A
missing card, a read-only card, or out-of-range values all fail safe back
to the hardcoded default.

## Display

Boot progress (firmware version, antenna-switch/radio status, active
channel, and any FATAL error) is also shown on the built-in LCD, not just
over serial — a one-shot status splash, not an interactive UI (that's
Phase 6). See `PROGRESS.md` if it doesn't render correctly on your unit;
the panel pins/offsets are sourced from a reference project, not
independently bench-verified by this repo yet.
