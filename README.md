<div align="center">

# LoRaTrace RX

**Passive LoRa and sub-GHz field logging**

[![Build](https://img.shields.io/github/actions/workflow/status/d3mocide/LoRaTrace-RX/build.yml?branch=main&label=build)](https://github.com/d3mocide/LoRaTrace-RX/actions/workflows/build.yml)
[![Last commit](https://img.shields.io/github/last-commit/d3mocide/LoRaTrace-RX?color=orange)](https://github.com/d3mocide/LoRaTrace-RX/commits/main)
[![Latest dev build](https://img.shields.io/github/v/tag/d3mocide/LoRaTrace-RX?label=dev-latest&color=6aa84f)](https://github.com/d3mocide/LoRaTrace-RX/releases/tag/dev-latest)
[![Code size](https://img.shields.io/github/languages/code-size/d3mocide/LoRaTrace-RX)](https://github.com/d3mocide/LoRaTrace-RX)

RX-only LoRa/sub-GHz wardriving firmware for the M5Stack Cap LoRa-1262
(SX1262) riding on a Cardputer-Adv (ESP32-S3). GPS-tagged detection
logging across four mission profiles — Meshtastic, MeshCore, Reticulum,
and general LoRa/spectrum exploration — built like a field instrument,
not a laptop-tethered tool.

</div>

---

> [!NOTE]
> **Receive-only, by design.** LoRaTrace has no transmit path beyond what
> the antenna-switch init requires, and never will (`CLAUDE.md` house
> rule). It observes and GPS-tags radio activity already in the air —
> including other people's mesh traffic — for later analysis. It does not
> inject, transmit, or decrypt anything not already in the clear. Operate
> it the way you'd operate any RF-monitoring instrument: know your local
> regulations and respect reasonable expectations of privacy.

## Table of contents

1. [Features](#features)
2. [Version](#version)
3. [Documentation](#documentation)
4. [Build](#build)
5. [Install without flashing (M5Launcher)](#install-without-flashing-m5launcher)
6. [Configuration](#configuration)
7. [Output files](#output-files)
8. [Display and controls](#display-and-controls)
9. [Web dashboard](#web-dashboard)

## Features

- **Four mission profiles**, one receiver: Meshtastic, MeshCore, Reticulum,
  and General Exploration (`Spectrum` on-device) — presets, not separate
  tools.
- **Three radio modes**, all GPS-tagged and logged to SD: **Watch**
  (continuous single-channel RX), **Probe** (bounded discovery scan across
  the full 868–923MHz band), and **Sweep** (frequency-binned energy map,
  with CAD confirmation at peaks landing in Phase 9).
- **GPS-stamped CSV logging**, one directory per wardrive — see
  [Output files](#output-files).
- **On-device menu UI** on the Cardputer-Adv's own screen and keyboard —
  no laptop required in the field. See
  [Display and controls](#display-and-controls).
- **On-demand WiFi dashboard** for live status, per-run CSV download, and
  channel configuration — off by default, never running unattended unless
  toggled on. See [Web dashboard](#web-dashboard).
- **No PSRAM, no problem.** Runs entirely within the ESP32-S3FN8's static
  RAM budget — SD is the datastore, not RAM. See
  [docs/DESIGN.md](docs/DESIGN.md).

## Version

No tagged `vX.Y.Z` release has shipped yet — `main` builds a rolling
[`dev-latest`](https://github.com/d3mocide/LoRaTrace-RX/releases/tag/dev-latest)
prerelease on every merge (see [Install](#install-without-flashing-m5launcher)
below), which is what the **dev-latest** badge above tracks. The firmware's
own semantic version lives in `src/version.h` and is bumped by hand when a
build-order phase lands — see
**[docs/STATUS.md](docs/STATUS.md)** for the current version, what's
hardware-verified, and what's still open, rather than a number restated
here that can drift out of sync.

## Documentation

| Doc | For |
|---|---|
| **[docs/STATUS.md](docs/STATUS.md)** | Current version, what's hardware-verified, what's still open. Start here. |
| [docs/DESIGN.md](docs/DESIGN.md) | Hardware, RF parameters, and architecture rationale — read before making architecture changes. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Build-order phases, MVP-Beta scope, and an honest feasibility read against this hardware's real limits. |
| [docs/LOG_GUIDE.md](docs/LOG_GUIDE.md) | Operator guide to run folders, CSV fields, identity observations, health checks, and privacy-aware export. |
| [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md) | Repeatable device-validation matrix and Phase 7 memory acceptance rules. |
| [docs/BRAND.md](docs/BRAND.md) | Naming, tone, and on-device UI copy conventions. |
| [CLAUDE.md](CLAUDE.md) | House rules for AI-assisted development on this repo. |
| [docs/README.md](docs/README.md) | Full documentation index, including archived history and research notes. |

## Build

PlatformIO + Arduino framework:

```sh
pio run -e cardputer-adv
pio run -e cardputer-adv --target upload
pio device monitor
```

Unit tests (host-native, no board needed):

```sh
pio test -e native
```

## Install without flashing (M5Launcher)

If your Cardputer-Adv already runs
[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher), you don't
need to touch USB flashing at all:

1. Download the latest build:
   `https://github.com/d3mocide/LoRaTrace-RX/releases/download/dev-latest/LoRaTraceRX-dev.bin`
   (rebuilt automatically from `main` on every merge — check that release's
   notes for the commit it came from). Tagged, more-stable versions will
   appear on the
   [Releases page](https://github.com/d3mocide/LoRaTrace-RX/releases) once
   one has shipped.
2. Copy the `.bin` onto a FAT32 SD card.
3. In Launcher: SD → select the file → Install.
4. To get back to Launcher afterward: reset the device and press any key
   during Launcher's own ~5s boot window (it prints "Press the button to
   enter the Launcher!" over serial while waiting) — miss it and Launcher
   auto-boots straight back into whatever ran last. To skip that timing
   window entirely, enable Launcher's own Settings → "Boot to Launcher"
   toggle; it then always stops at its menu on reset until you turn it
   back off. Not a software hook this firmware implements — see
   `docs/history/PROGRESS.md` for how this was confirmed against
   Launcher's own source.

Serial output still works normally over USB while running under Launcher
— `pio device monitor` to watch the boot banner and any `[RX]`/`[config]`
lines.

## Configuration

By default LoRaTrace RX locks to Meshtastic LongFast (US):
906.875MHz / SF11 / BW250kHz / CR4:8, and MeshCore's US-narrow default:
910.525MHz / SF7 / BW62.5kHz / CR5. The first time it boots with an SD
card that doesn't already have one, it creates `/loratrace/config.txt` on
the card pre-filled with both defaults, one block per profile
(`meshtastic_freq_mhz=`, `meshcore_freq_mhz=`, etc. — each profile has its
own independent set of keys, so editing one never touches the other's
saved values). Edit either block in place for a non-default regional
preset (e.g. MeshOregon) and reboot — see the file's own comments for the
format, or use the web dashboard's Settings tab instead of hand-editing
(below). (`sd-template/loratrace/` still exists if you want to prepare a
card offline before ever booting the device with it.) A missing card, a
read-only card, or out-of-range values all fail safe back to the hardcoded
default for that profile. Channel changes apply on next boot, not live —
switching profiles on the running device (menu or web) always uses
whatever was last saved for that profile.

## Output files

**One wardrive is one directory.** Each power-on claims the next free
`/loratrace/runNNNN/` on the card, so a drive can be copied, shared or
deleted as a unit:

```
/loratrace/
  config.txt          channel override (not a run artifact)
  run0001/
    detections.csv
    nodes.csv
    session.csv
    probe.csv
    energy.csv
  run0002/
    ...
```

- **`detections.csv`** — one row per received packet: GPS-stamped, with
  RSSI/SNR, RF parameters and whatever routing metadata the protocol
  exposes in clear. The full frame is retained as hex for offline analysis;
  LoRaTrace does not claim a general payload decoder.
- **`session.csv`** — one health row a minute (packets, drops, worst SD bus
  hold, heap free *and* low-water, GPS state, battery), plus a row marking
  the start. An unattended run is judged on whether it held up, and nobody
  is watching the serial console at hour three — so the run records its own
  vital signs next to its findings.
- **`nodes.csv`** — supported Meshtastic NodeInfo and MeshCore advertisement
  identity observations, kept separate because many packets can belong to one
  node. See `docs/LOG_GUIDE.md` for the exact supported profiles and limits.
- **`probe.csv`** — written when a Probe (discovery scan) is run: which
  fixed-candidate channels produced CAD activity. A CAD hit is not
  necessarily a packet.
- **`energy.csv`** — written when a Sweep (energy scan) is run: sparse
  high-energy bins and follow-up CAD results. Not a full spectrum
  recording.

`probe.csv` and `energy.csv` may exist with only a header row if that
mode was never triggered during the run — their absence of data is not an
error. See `docs/LOG_GUIDE.md` for the full field-by-field schema of every
file above.

Runs are numbered rather than timestamped because the name has to be chosen
before the GPS knows what time it is, and this board has no verified RTC.
The wall clock still reaches the card, recorded inside the run once a fix
lands. `docs/LOG_GUIDE.md` is the operator reference for schemas and analysis;
`docs/DESIGN.md` §8 explains the design rationale. The current run number is shown
on the RADIO page as `r<N>`.

## Display and controls

Boot progress (firmware version, antenna-switch/radio status, active
channel, and any FATAL error) is shown on the built-in LCD as well as over
serial. Once the tasks are running the panel shows four read-only status
pages — RADIO, CHANNEL, GPS, SYSTEM — with a battery indicator and header
status dots (GPS fix, heap health, live RX activity) on every one, plus a
footer line showing the active profile and page position.

A keyboard-driven, nestable menu covers every operator-facing toggle, three
root rows deep: **Trace** (pause/standby the radio-listening pipeline
without losing GPS fix), **Profile** (Meshtastic / MeshCore / Node IDs
capture), and **System** (WiFi and Serial Control under Connectivity,
Debug and SD retry under Diagnostics, brightness and idle-dim under
Display). `,`/`.` cycle pages or move the current menu level's selection,
digits `1`-`5` jump straight to a page, Enter acts on the highlighted row
(or scrubs a slider), and the backtick/ESC key opens/closes the menu and
steps back up one level at a time; with no keyboard detected the pages
rotate on their own, so a device sitting on a dashboard still cycles
through everything. A toast band confirms whatever action just fired.

## Web dashboard

An on-demand WiFi access point (off by default — toggle it from the
System menu, never running unattended during a drive unless you turn it
on) hosts a small embedded web page at `192.168.4.1` once it's up. Three
tabs: a live status dashboard (the same counters the on-device SYSTEM/
RADIO pages and serial `[status]` line already expose, plus whether Trace
is active or paused), a run browser to download any of `detections.csv`/
`session.csv`/`nodes.csv`/`probe.csv`/`energy.csv` per run without ejecting
the SD card, and a Settings tab with
one independent panel per profile for editing channel parameters — the
same `config.txt` the SD card holds, applied on next boot. Measured cost:
roughly 55-60KB of heap while the AP is up, with no impact on radio/GPS/SD
reliability under real traffic — see `docs/history/PROGRESS.md` for the
measurement session, or `docs/STATUS.md` for what's still open.
