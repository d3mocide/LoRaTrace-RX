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
4. To get back to Launcher afterward: manual restart + button combo (not
   a software hook this firmware implements).

Serial output still works normally over USB while running under Launcher
— `pio device monitor` to watch the boot banner and any `[RX]`/`[config]`
lines.

## Configuration

By default LoRaTrace RX locks to Meshtastic LongFast (US):
906.875MHz / SF11 / BW250kHz / CR4:8. To use a non-default regional preset
(e.g. MeshOregon), copy `sd-template/loratrace/` to the root of your SD
card and edit `config.txt` with your mesh's actual values — see that
file's comments for the format. A missing card, missing file, or
out-of-range values all fail safe back to the hardcoded default.
