# LoRaTrace — Design Doc

RX-only LoRa/sub-GHz wardriving firmware for the M5Stack Cap LoRa-1262 (SX1262)
riding on a Cardputer-Adv (ESP32-S3). GPS-tagged detection logging across four
mission profiles: Meshtastic, MeshCore, Reticulum, and general LoRa/spectrum
exploration.

## 1. Target hardware & resource budget

**Host:** Cardputer-Adv — ESP32-S3FN8 (dual-core Xtensa LX7 @ 240MHz), **8MB
flash, no PSRAM**, 1.14" ST7789V2 LCD (240×135), 56-key keyboard, microSD slot.

**Radio:** Cap LoRa-1262 — SX1262, 868–923MHz tuned front end, FSK/GFSK/MSK/
LoRa/OOK, external RP-SMA antenna, onboard GNSS (ATGM336H per M5Stack's
official Cap LoRa-1262 docs/pin diagram — this doc previously said
"AT6668"; naming correction only, NMEA UART either way, no pin/code
impact).

**Resource implication:** no PSRAM means ~512KB SRAM total, shared with the
RTOS and whatever else is linked in. Realistic free heap is probably 250–380KB
— confirm with `ESP.getFreeHeap()` once flashing real code, don't trust a
number on paper. **microSD is the datastore. RAM is a relay buffer, not a log.**
Keep the in-RAM detection queue small (ring buffer, ~40B/entry) and flush to
SD promptly rather than accumulating.

**Pin map:**

| Peripheral | Interface | Pins | Task owner |
|---|---|---|---|
| SX1262 | SPI (shared with microSD, see below) | NSS G5, SCK G40, MOSI G14, MISO G39, IRQ G4, BUSY G6, RST G3 | Radio task |
| PI4IOE5V6408 IO expander | I2C, addr 0x43 | SDA G8, SCL G9 | Boot init only |
| GPS (ATGM336H) | UART @ 115200 8N1 | RX G13, TX G15 | GPS task |
| microSD | SPI, same bus as SX1262 (CS G12, shared SCK/MOSI/MISO — §7) | CS G12 | Logger task |
| ST7789V2 LCD + keyboard | SPI + GPIO matrix | internal | UI task |

**Startup requirement:** the antenna path is gated by an IO expander — **P0 on
the PI4IOE5V6408 must be driven high once at boot** or the radio hears
nothing regardless of firmware correctness. This is a one-time init step, not
a per-RX toggle.

**Bus isolation:** give the SX1262 its own SPI host, separate from the
display. Display refresh transactions on a shared bus will jitter CAD
timing. Note this is isolation from the *display* specifically — SD
shares the SX1262's bus by hardware design (§7), which is a different,
real constraint on Logger task timing; don't assume "own SPI host" means
isolated from SD too.

**Frequency coverage caveat:** the module's RF front end is tuned 868–923MHz.
US ISM is 902–928MHz. The top ~5MHz of the US band (923–928) is outside the
tuned range — expect reduced sensitivity there. The good news: every mission
profile's known/likely channel (see §4) falls inside 868–923, so this mostly
matters for the General Exploration profile's full-band sweep. Confirm with
an empirical RSSI noise-floor sweep once hardware is in hand.

## 2. Task / core architecture

```
                    ┌───────────────────────────────┐
                    │  Core 1 (APP_CPU) — dedicated   │
  SX1262 ──SPI-A──► │  Radio Task (highest priority)  │
                    │   • mission-profile state machine│
                    │   • CAD / RX, never blocks       │
                    └───────────────┬─────────────────┘
                                    │ detection_t (~40B) via
                                    │ FreeRTOS queue
                    ┌───────────────▼─────────────────┐
                    │  Core 0 (PRO_CPU) — support       │
  GPS ────UART────► │   GPS Task → last-fix (mutex)     │
  microSD ──SPI───► │   Logger Task ← dequeues, stamps  │
                    │     GPS fix + profile, batches SD │
  LCD+keys ──SPI──► │   UI Task → redraw, key input      │
                    │   [optional] WiFi Upload Task      │
                    └───────────────────────────────────┘
```

Radio task never touches SD or the display directly — it drops a struct in a
queue and moves on. SD writes are the one operation on this board with real
latency spikes; keep them off the radio task entirely.

## 3. Mission profiles — RF parameters

| Profile | Frequency | SF | BW | CR | Confidence |
|---|---|---|---|---|---|
| **Meshtastic** (US LongFast default) | 906.875 MHz (slot 20 of 104) | 11 | 250 kHz | 4/8 | High — official docs, multiple corroborating sources |
| **MeshCore** (US/Canada "Recommended", post-Oct-2025 "narrow" migration) | 910.525 MHz | 7 | 62.5 kHz | 4/5 | High — MeshCore's own FAQ + multiple community sites agree |
| **MeshCore** (legacy, pre-migration, may still be in the wild) | ~915 MHz region | 11 | 250 kHz | — | Medium — worth a fallback discovery pass |
| **Reticulum** | **No standard exists.** Communities deliberately pick arbitrary settings, often specifically offset from Meshtastic/MeshCore to avoid collision (one documented example uses 869.463MHz/SF8 specifically to dodge 868.0MHz) | — | — | — | N/A by design |
| **General LoRa / possible LoRaWAN** | Full 868–923MHz | any | any | any | Discovery only |

**MeshCore's channel model is simpler than Meshtastic's:** it does not use a
slot-hashing multi-channel scheme. In practice the whole regional community
converges on one shared frequency/SF/BW/CR; logical separation happens via
"rooms" and addressing, not separate RF channels. One number to know per
region, not 104.

**Reticulum has no fixed target by design** — this profile is fundamentally a
discovery problem, not a tuning problem.

**Cross-reference your own data before hardcoding anything:** [[meshmapper-pipeline]]
already has real-world MeshCore-frequency observations for this area. Check
it before trusting a scraped "US default" number.

## 4. Two engine families

The four profiles reduce to two underlying scan strategies, sharing the same
radio task infrastructure:

**A. Known-channel family (Meshtastic, MeshCore)** — `HOME_LISTEN` dominant.
Because neither protocol frequency-hops and MeshCore in particular has one
regional frequency, sitting in continuous RX on the known/likely channel
catches nearly everything without any cycling. `DISCOVERY_SWEEP` runs
occasionally to catch non-default Meshtastic channel-name hashes or
legacy-config MeshCore stragglers.

**B. Discovery family (Reticulum, General Exploration)** — `ENERGY_SWEEP` /
`CAD_SWEEP` dominant. No known target, so the strategy is a protocol-agnostic
RSSI sweep across the full band (catches any emitter, not just LoRa) layered
with LoRa-mode CAD checks at a handful of common SF/BW combinations (7/8/9 ×
125/250/500/62.5kHz) to flag likely LoRa activity specifically. A useful
heuristic for Reticulum specifically: a LoRa CAD hit at a frequency that does
**not** match a known Meshtastic/MeshCore channel is a Reticulum (or
unclassified private LoRa) candidate, since Reticulum operators deliberately
avoid those frequencies.

## 5. Radio task state machine

- `INIT` — bring up SX1262 on its own SPI host, set IO-expander P0 high,
  mount SD, load channel tables
- `HOME_LISTEN(profile)` — continuous RX locked to the profile's known
  channel; on valid packet → attempt decode if cleartext/known-key → push
  detection event → stay locked
- `DISCOVERY_SWEEP(profile)` — bounded-duration (a few seconds), CAD-cycles
  a curated candidate list for the active profile; logs hits; returns to
  `HOME_LISTEN`
- `ENERGY_SWEEP` — top-level mode, mutually exclusive with the above two;
  FSK/OOK RSSI sweep across 868–923MHz plus periodic LoRa-mode CAD checks
  at common SF/BW combos; this state *is* the Reticulum and General
  Exploration profiles

Mission profile is operator-selected (keyboard), not auto-detected. Switching
profiles reconfigures which channel table `HOME_LISTEN`/`DISCOVERY_SWEEP`
pull from; the state machine shape doesn't change.

## 6. Protocol fingerprinting (post-hoc classification)

Every captured packet — regardless of which profile was active when it hit —
gets tagged with a best-guess classification for later analysis:

| Signal | Likely protocol |
|---|---|
| SF11/BW250 at a Meshtastic channel-hash-derived frequency | Meshtastic |
| SF7/BW62.5 near 910.525MHz (or other narrow-BW cluster) | MeshCore (current) |
| SF11/BW250 near 915MHz, off the Meshtastic slot grid | MeshCore (legacy) — ambiguous, check sync word |
| Sync word 0x34 | LoRaWAN (public) |
| LoRa CAD hit, frequency not matching known Meshtastic/MeshCore channels | Reticulum candidate / unclassified private LoRa |

**Do not hardcode exact sync-word register values yet** — see §7, this needs
source-level verification, not a scraped number.

## 7. Known unknowns — verify before / during build

- [ ] **Meshtastic's exact SX126x sync-word register value.** Sources
      disagree (0x2B vs. its two-byte register mapping vs. 0x12 cited
      elsewhere as "Meshtastic private"). Pull this from Meshtastic firmware
      source or RadioLib's own Meshtastic-compat example, not a blog post.
- [ ] **MeshCore's encryption/PSK scheme.** Don't assume it mirrors
      Meshtastic's default-channel PSK model — MeshCore's own docs
      explicitly warn against importing Meshtastic preset assumptions.
- [x] **microSD bus** — SPI or SDMMC, and whether it shares a host with the
      display, on this specific Cardputer-Adv revision. **Bench-confirmed
      (2026-08-22):** first real-hardware boot log shows `SD.begin(PIN_SD_CS,
      radioSPI)` succeeding on the shared bus (the code takes a visibly
      different, "no SD card detected" path when the mount itself fails —
      the log instead shows the *file-not-found* path, which is only
      reachable after a successful mount). Bus-level arbitration for
      concurrent access (next paragraph) is still unverified — this only
      confirms the sequential setup()-time mount works. **Finding
      (2026-08-12), well-sourced but not bench-confirmed:** cross-checked
      M5Stack's own official docs pages directly — the Cardputer-Adv
      base unit's microSD pin table (CS=G12/SCK=G40/MOSI=G14/MISO=G39) and
      the Cap LoRa-1262's own SPI pin table + printed pin-diagram image
      (NSS=G5/SCK=G40/MOSI=G14/MISO=G39, matching this doc's §1 table).
      Identical SCK/MOSI/MISO, distinct CS — SD and the LoRa radio share
      one physical SPI bus. The Cardputer-Adv docs state outright that the
      microSD interface shares pins with the EXT/Cap expansion connector,
      which is the connector the LoRa Cap plugs into — that's the actual
      mechanism, not a numeric coincidence. Doesn't appear to share with
      the display. (Note: an initial, hastier fetch of these same pages
      produced a scrambled/self-contradictory pin table — re-fetched with
      a stricter "quote verbatim" prompt and cross-checked against a
      photo of the module's own printed diagram before trusting it.)
      Real consequence for §1's "radio task never blocks on SD" rule:
      moving SD to a Core 0 task prevents the radio task's *own code* from
      blocking, but doesn't remove the need for bus-level arbitration
      (e.g. a mutex around the shared SPI peripheral) once both are active
      concurrently — the FreeRTOS queue alone doesn't solve that. Tracked
      further in PROGRESS.md.
- [ ] **CAD `symNum` tuning** — the 2-symbol dwell estimates in the prior
      sketch are ballpark; real false-positive/miss tradeoff needs bench
      testing against Semtech's AN1200.48 guidance.
- [ ] **868–923MHz front-end rolloff**, especially near 923MHz — empirical
      RSSI floor sweep once hardware arrives.
- [ ] **Exact per-slot frequency spacing for Meshtastic's 104 US slots** —
      ~250kHz apart is a reasonable inference from LongFast's own channel
      bandwidth, but pull the real table from firmware source rather than
      deriving it.

## 8. SD log schema

`timestamp_utc, lat, lon, fix_quality, profile, freq_mhz, sf, bw_khz, rssi_dbm, snr_db, classification, decoded, channel_or_node_id, raw_len`

`ENERGY_SWEEP` data gets threshold-filtered against a rolling noise floor
before logging — don't dump every sweep point, only peaks, or the card fills
fast for near-zero value.

## 9. Build order

1. Bring-up: RadioLib talking to the SX1262 on its own SPI host, IO-expander
   P0 confirmed, hardcoded RX on 906.875/SF11/BW250, print to serial
2. `HOME_LISTEN` + Logger task + GPS fusion + SD writes — Meshtastic War
   Drive is functionally complete at this point
3. Add MeshCore profile (910.525/SF7/BW62.5/CR5) — same engine, new table
4. `DISCOVERY_SWEEP` with curated candidate lists per profile, weighted by
   MeshMapper-observed frequencies where available
5. `ENERGY_SWEEP` — General Exploration and Reticulum profiles
6. UI polish, optional WiFi upload task last (biggest RAM/RF-noise cost)

## 10. References

- Meshtastic radio settings & presets: meshtastic.org/docs/overview/radio-settings
- MeshCore FAQ (region presets, Oct-2025 narrow migration): github.com/meshcore-dev/MeshCore/blob/main/docs/faq.md
- Reticulum interface config (no fixed LoRa standard): markqvist.github.io/Reticulum/manual/interfaces.html
- RadioLib SX126x CAD / channel scan API: jgromes.github.io/RadioLib
- M5Stack Cap LoRa-1262 pinout/schematics: docs.m5stack.com/en/cap/Cap_LoRa-1262
