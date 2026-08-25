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

**Phase 1 complete and hardware-verified**: radio, antenna path, SD config
override, display, and protocol-correct Meshtastic RX all confirmed on real
hardware, with heap stable under sustained receive.

**Phase 2 (MVP-Beta) built and largely hardware-verified**: the
task/queue architecture, GPS task and batched SD logger are in place, and
the GPS now reaches a fix on this board. What remains is the exit criterion
itself — a multi-hour unattended run whose logs come back clean. See
`PROGRESS.md` for the live checklist.

## Output files

**One wardrive is one directory.** Each power-on claims the next free
`/loratrace/runNNNN/` on the card, so a drive can be copied, shared or
deleted as a unit:

```
/loratrace/
  config.txt          channel override (not a run artifact)
  run0001/
    detections.csv
    session.csv
  run0002/
    ...
```

- **`detections.csv`** — one row per received packet: GPS-stamped, with
  RSSI/SNR, RF parameters and whatever routing metadata the protocol
  exposes in clear. Payloads are encrypted and stay that way; this is a
  passive receiver, not a decoder.
- **`session.csv`** — one health row a minute (packets, drops, worst SD bus
  hold, heap free *and* low-water, GPS state, battery), plus a row marking
  the start. An unattended run is judged on whether it held up, and nobody
  is watching the serial console at hour three — so the run records its own
  vital signs next to its findings.

Runs are numbered rather than timestamped because the name has to be chosen
before the GPS knows what time it is, and this board has no verified RTC.
The wall clock still reaches the card, recorded inside the run once a fix
lands. Full schemas and the arithmetic for dating a run are in `DESIGN.md`
§8. The current run number is shown on the RADIO page as `r<N>`.

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
channel, and any FATAL error) is shown on the built-in LCD as well as over
serial. Once the tasks are running the panel shows five read-only status
pages — RADIO, CHANNEL, GPS, SYSTEM, WIFI — with a battery indicator and a
heartbeat dot on every one, plus a keyboard-driven menu (profile switch,
WiFi toggle, verbose debug toggle). `,`/`.` cycle pages or move the menu
selection, digits `1`-`5` jump straight to a page, Enter acts on the
highlighted menu row, and the backtick/ESC key opens/closes the menu; with
no keyboard detected the pages rotate on their own, so a device sitting on
a dashboard still cycles through everything.

A UI architecture redesign (grouped menu, more toggles, reorganized status
pages) is Phase 6 — see ROADMAP.md.
