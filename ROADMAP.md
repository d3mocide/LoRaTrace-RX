# LoRaTrace RX — Roadmap

This roadmap operationalizes the build order already decided in
`DESIGN.md` §9. It doesn't change any RF or architecture decision — it adds
scope boundaries, exit criteria, and an honest read on what this specific
hardware can and can't do, so "MVP-Beta" means something concrete instead
of a vibe.

## What "MVP-Beta" means here

The smallest version of this firmware that's actually useful as a field
tool: **Meshtastic War Drive, end to end.** RX locked to the LongFast (US)
channel, every detection GPS-stamped and written to SD, running
unattended on battery. That's DESIGN.md §9 phases 1–2. Everything past
that (MeshCore, discovery sweeps, energy sweep, UI, upload) is real
project scope, but it's post-MVP-Beta.

## Hardware feasibility — reality check

Read this before assuming any phase below "just works." It's an honest
accounting against the ESP32-S3FN8's actual limits (CLAUDE.md hardware
assumptions), not a pitch.

**Solid — low risk:**
- RX-only detection on a known channel (`HOME_LISTEN`) for Meshtastic and
  MeshCore. Continuous single-channel RX is cheap: no polling, no
  scanning, minimal CPU. This is the easy 80% of the project's value.
- The Core 1 (radio) / Core 0 (GPS+SD+UI) split with a FreeRTOS queue.
  Standard ESP32-S3 pattern, and it's the right call given SD's latency
  spikes — keeping those off the radio task is the one architectural
  decision that actually matters for not dropping packets.
- GPS fusion (NMEA over UART) and small-struct SD logging. Neither needs
  meaningful RAM; TinyGPSPlus-class parsers and a ~40B detection struct
  flushed often are well inside an 8MB-flash, no-PSRAM budget.
- Keyboard + small-display UI, *if* it avoids a full framebuffer (see
  below).

**Achievable, but needs deliberate care:**
- **No PSRAM means ~512KB SRAM total, shared with the RTOS, WiFi stack (if
  enabled), display driver, and every task's stack.** DESIGN.md's own
  estimate of 250–380KB free heap is a guess pending `ESP.getFreeHeap()`
  on real hardware — treat every RAM-hungry decision below as provisional
  until that number is in hand.
- **Display:** a naive full framebuffer at 240×135×16bpp is ~65KB — a
  meaningful bite out of a few-hundred-KB heap. Use direct-to-panel
  partial-window writes (LovyanGFX/M5GFX style) instead of buffering a
  whole frame, especially once WiFi is in the picture.
- **`ENERGY_SWEEP` (Reticulum / General Exploration):** a single SX1262
  cannot listen to the whole 868–923MHz band at once. The design's
  sweep-then-return-to-listen approach is the right shape for this
  hardware, but it's a fundamental physical tradeoff, not a bug to code
  around: short bursts on a non-home frequency *will* be missed during the
  portion of the duty cycle spent elsewhere. Set that expectation in the
  UI/docs rather than implying continuous full-band coverage.
- **Front-end rolloff near 923–928MHz:** the module's tuned range tops out
  around 923MHz; the top of US ISM (923–928MHz) is outside it. This is a
  hardware fact, not fixable in firmware — it specifically dents General
  Exploration's full-band sweep, since every named profile's actual
  channel already sits inside 868–923 (DESIGN.md §1). Needs an empirical
  RSSI noise-floor sweep once hardware is in hand, then document the
  sensitivity gap rather than pretend it isn't there.
- **Meshtastic's 104-slot hash space:** Phase 1–2 only lock to the
  LongFast US default (slot 20). That's most of the real-world traffic,
  but a mesh running a non-default channel name lands on a different slot
  and is invisible to `HOME_LISTEN`. `DISCOVERY_SWEEP` (phase 4) is what
  closes that gap — until then, be precise in docs/UI that "Meshtastic
  profile" means "default channel," not "all Meshtastic traffic."

**Genuinely open — blocks real functionality, not just polish:**
- microSD bus (SPI vs SDMMC) on this board revision — DESIGN.md §7,
  unresolved. Blocks finalizing the Logger task's pin/driver choice.
- Meshtastic's exact sync-word register value, and MeshCore's
  encryption/PSK model — DESIGN.md §7. Detection (RSSI/SF/BW/timing) works
  without these; reliable protocol-level filtering and payload decode
  don't. Don't hardcode a guessed value for either (CLAUDE.md house rule).

**Deliberately out of scope / lowest priority:**
- **WiFi upload task.** DESIGN.md itself flags this as "biggest RAM/RF-
  noise cost" and puts it last. On a no-PSRAM chip already running radio +
  GPS + SD + display tasks, adding lwIP/WiFi's heap footprint is the
  single likeliest thing to blow the RAM budget or introduce RF
  self-interference near the SX1262's front end. Treat as an experiment
  to run once everything else is stable and the real free-heap number is
  known, not a given.
- TX/injection of any kind — permanent non-goal per CLAUDE.md house rules,
  not a phase-ordering question.

## Phases

Each phase maps 1:1 to DESIGN.md §9. "Blocking unknowns" cross-reference
DESIGN.md §7 items that must be resolved (or explicitly deferred) before
the phase can be called done, not just started.

### Phase 0 — Project scaffold
**Status:** done (this change).
PlatformIO project, pin map, RF parameter tables, phase-1 bring-up code,
this roadmap, and the tracking doc.

### Phase 1 — RadioLib bring-up
**Goal:** prove the radio path is alive.
**Deliverable:** `src/main.cpp` — SPI init, IO-expander P0 high, SX1262 up,
hardcoded RX on 906.875MHz/SF11/BW250/CR8, detections printed to Serial.
**Exit criteria:** a real Meshtastic LongFast (US) packet in the air shows
up on the serial monitor with plausible RSSI/SNR.
**Blocking unknowns:** none functionally — but the IO-expander register
map and SPI host assignment (see PROGRESS.md) are unverified against real
hardware and are the first suspects if the radio stays silent.
**Status:** code written, **not yet run on hardware.**

### Phase 2 — `HOME_LISTEN` + task/queue architecture + GPS + SD (= MVP-Beta)
**Goal:** the smallest genuinely useful field tool.
**Deliverable:** `radio_task`, `gps_task`, `logger_task` per the proposed
layout in CLAUDE.md; FreeRTOS queue Core 1 → Core 0; GPS-stamped
detections batched to SD per the §8 log schema.
**Exit criteria:** unattended run — power on, GPS fix acquired, detections
logged with correct lat/lon, no dropped packets attributable to SD
latency, no crash from heap exhaustion over a multi-hour run.
**Blocking unknowns:** microSD bus (SPI vs SDMMC) must be confirmed before
the logger task's driver/pins are final.

### Phase 3 — MeshCore profile
**Deliverable:** same `HOME_LISTEN` engine, MeshCore US-narrow table
(910.525MHz/SF7/BW62.5/CR5) wired in as a second selectable profile.
**Blocking unknowns:** none for basic detection; MeshCore's
encryption/PSK model (§7) still blocks payload decode, not detection.

### Phase 4 — `DISCOVERY_SWEEP`
**Deliverable:** bounded-duration CAD-cycle sweep of a curated candidate
list per active profile — non-default Meshtastic slots, legacy MeshCore.
**Blocking unknowns:** curated candidate lists should be weighted by
MeshMapper-observed frequencies ([[meshmapper-pipeline]], per CLAUDE.md)
where available, not scraped defaults alone. CAD `symNum` tuning (§7)
needs bench testing against Semtech AN1200.48 before trusting false-
positive/miss rates.

### Phase 5 — `ENERGY_SWEEP`
**Deliverable:** Reticulum and General Exploration profiles — FSK/OOK RSSI
sweep across 868–923MHz with periodic LoRa CAD checks at common SF/BW
combos.
**Blocking unknowns:** 923–928MHz front-end rolloff should be
characterized (empirical RSSI floor sweep) so the UI can be honest about
reduced sensitivity in that sub-band rather than silently under-reporting.

### Phase 6 — UI polish, optional WiFi upload
**Deliverable:** full `ui_task` (profile switching, live status per
BRAND.md's on-device copy conventions), and — only if the RAM budget
supports it after phases 1–5 are real and measured — a WiFi upload task.
**Blocking unknowns:** requires a real `ESP.getFreeHeap()` number under
full load (radio + GPS + SD + display all running) before deciding WiFi is
in scope at all. This phase is explicitly allowed to conclude "not on this
hardware, or not simultaneously with the display."

## Suggested versioning

| Version | Corresponds to |
|---|---|
| v0.1 | Phase 1 (serial bring-up) |
| v0.2 | Phase 2 (MVP-Beta: Meshtastic War Drive complete) |
| v0.3 | Phase 3 (MeshCore) |
| v0.4 | Phase 4 (discovery sweep) |
| v0.5 | Phase 5 (energy sweep: Reticulum + General Exploration) |
| v1.0 | Phase 6 (UI polish, WiFi upload decision made, all 4 profiles stable) |

## Non-goals

- Transmit or injection of any kind, beyond the one-time antenna-switch
  init GPIO write. Permanent, per CLAUDE.md house rules — not a phase to
  eventually reach.
- Auto-detecting mission profile. Operator-selected via keyboard, by
  design (DESIGN.md §5).
- Full protocol decode/decrypt as an MVP-Beta requirement — depends on the
  two genuinely-open unknowns above (sync word, MeshCore PSK model) and
  isn't needed for the core "detect and log" value proposition.
