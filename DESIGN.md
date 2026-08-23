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
| ST7789V2 LCD | SPI, own host, isolated from the radio/SD bus above | DC G34, CS G37, SCK G36, MOSI G35, RST G33, BL G38 | UI task (Phase 6); boot-status splash only for now — PROGRESS.md |
| Keyboard | Cardputer-ADV: TCA8418 I2C controller, addr 0x34, same SDA/SCL (G8/G9) as the IO expander above, different address — not the plain GPIO matrix the base (non-ADV) Cardputer uses | G8/G9 (ADV) | UI task (Phase 6, not wired up yet) |

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

**Resolved for Meshtastic (2026-08-23), still open for MeshCore** — see §7.
Meshtastic's value is now verified from upstream firmware source and set in
`channel_plans.h`; MeshCore's is not, and must not be guessed.

## 7. Known unknowns — verify before / during build

- [x] **Meshtastic's exact SX126x sync-word register value.** Sources
      disagreed (0x2B vs. its two-byte register mapping vs. 0x12 cited
      elsewhere as "Meshtastic private"). **Resolved 2026-08-23 from
      upstream firmware source** — meshtastic/firmware
      `src/mesh/RadioLibInterface.h`: `const uint8_t syncWord = 0x2b;`. Its
      own comment explains the disagreement rather than just contradicting
      it: *"For releases before 1.2 we used 0x12 (or for very old loads
      0x14). Note: do not use 0x34 - that is reserved for lorawan. We now
      use 0x2b ... We will be staying with this code for a long time."* So
      0x12 was **stale, not wrong** — it's pre-1.2 Meshtastic, and it also
      happens to be RadioLib's own `RADIOLIB_SX126X_SYNC_WORD_PRIVATE`
      default (verified in RadioLib's `SX1262.h` `begin()` signature), which
      is how this firmware ended up silently listening on it. The
      "two-byte register mapping" ambiguity dissolves too: callers pass the
      **one-byte** value to RadioLib's `begin()`/`setSyncWord()` and RadioLib
      does the two-byte register mapping internally — Meshtastic itself uses
      that same RadioLib API, so passing 0x2B matches it exactly. The 0x34 →
      LoRaWAN note independently corroborates §6's fingerprint table.
      Bench-confirmation that this actually recovers Meshtastic RX is still
      pending (see PROGRESS.md).
- [ ] **MeshCore's encryption/PSK scheme.** Don't assume it mirrors
      Meshtastic's default-channel PSK model — MeshCore's own docs
      explicitly warn against importing Meshtastic preset assumptions.
      (Note: this is still open. MeshCore's *sync word* was resolved
      2026-08-23 — see below — but that says nothing about its crypto.)
- [x] **MeshCore's SX126x sync word.** **Resolved 2026-08-23** from
      upstream source (repo now at `meshcore-dev/MeshCore`):
      `src/helpers/radiolib/CustomSX1262.h` passes
      `RADIOLIB_SX126X_SYNC_WORD_PRIVATE` — **0x12**. Notably this is
      RadioLib's stock default, so unlike Meshtastic, MeshCore RX was never
      broken by this firmware's missing `setSyncWord()` call. Now set
      explicitly as a cited `SYNC_WORD_MESHCORE` in `channel_plans.h` so
      it's a verified fact rather than a lucky coincidence.
- [ ] **Preamble length.** Both protocols transmit with preamble length 16
      (Meshtastic `RadioInterface.h`: *"8 is default, but we use longer to
      increase the amount of sleep time when receiving"*; MeshCore's
      `CustomSX1262.h` passes 16). This firmware runs RadioLib's default of
      8, and that empirically does **not** block RX — live Meshtastic
      frames decode fine at 8, confirmed on hardware 2026-08-23. Left as-is
      deliberately: continuous RX syncs on whatever preamble arrives, so
      this only bites the duty-cycled/CAD scanning in §4. Re-evaluate
      during phase 4 with a bench test, not before.
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
- [ ] **ST7789V2 LCD pins/panel offsets (§1 table)** — sourced 2026-08-22
      from `bmorcelli/Launcher`'s own confirmed-working Cardputer/
      Cardputer-ADV build config (`boards/m5stack-cardputer/platformio.ini`
      in that repo — device name is literally "M5Stack Cardputer & ADV",
      i.e. one shared display config for both variants), not derived or
      guessed. Not independently bench-verified against this board yet —
      see PROGRESS.md's boot-status-splash entry and Phase 1 checklist.

## 8. SD log schema

**One wardrive is one directory.** Each power-on claims the next free
`/loratrace/runNNNN/` and writes both its files there:

```
/loratrace/
  config.txt              channel override, not a run artifact
  run0001/
    detections.csv
    session.csv
  run0002/
    ...
```

A drive can then be copied, shared, imported or deleted as a unit. The
alternative — one continuous file every power-on appends to — is durable but
not usable: the operations an operator actually performs on a drive all
become text-editing chores.

**Why an index rather than a timestamp in the name.** The name must be
decided the moment logging starts, and at that moment the device does not
know the time: absolute time comes from GPS, this board has no verified RTC,
and a cold TTFF is tens of seconds at best. Timestamp naming would mean
either delaying file creation (losing every packet heard during TTFF) or
renaming later (leaving a provisional name behind on any power cut before
the rename). An index is knowable immediately, needs no clock, and is stable
under power loss. The wall clock still reaches the card — recorded *inside*
the run on the first health row that has a fix — which dates the run without
ever having gated its creation on a clock.

The next index comes from scanning the card for the highest `runNNNN`, not
from a stored counter: the listing is the truth, it cannot drift, and there
is no mutable state to corrupt on a power cut. Scanning for the highest (not
the first gap) means a deleted run's number is never silently reused.

Both files below live inside that run directory.

### 8.1 `detections.csv` — the mission data

`timestamp_utc, lat, lon, fix_quality, profile, freq_mhz, sf, bw_khz, rssi_dbm, snr_db, classification, decoded, channel_or_node_id, raw_len, rx_uptime_ms, run`

`rx_uptime_ms` is device uptime at the moment of reception. It is the only
time reference a detection heard *before the first GPS fix* has — those rows
carry an empty `timestamp_utc` and empty coordinates, and without uptime
they could not be placed in time at all. It also exposes queue backlog (a
row whose GPS stamp is much later than its uptime was stamped late) and is
the join key to `session.csv`.

`ENERGY_SWEEP` data gets threshold-filtered against a rolling noise floor
before logging — don't dump every sweep point, only peaks, or the card fills
fast for near-zero value.

### 8.2 `session.csv` — the run's own vital signs

`timestamp_utc, uptime_s, reason, lat, lon, sats, sats_in_view, fix_type, ttff_s, rx, crc_err, queue_drop, bus_miss, rows, row_drop, flushes, max_flush_ms, max_session_ms, sd, bus_contention, nmea, nmea_bad_crc, heap_free, heap_min, batt_mv, logger_stack_free, run, gps_max_loop_gap_ms, gps_oversize_drops`

One row per minute, plus a `reason=boot` row when the card comes up.

This exists because §9 phase 2's exit criterion is an *unattended* run — no
dropped packets attributable to SD latency, no heap exhaustion over hours.
Every counter that settles that claim was already exposed for the serial
status line and the on-screen pages, and both need someone watching. A
device driven around on battery and unplugged at the end kept no record of
its own health, so the one run the criterion actually describes was the one
run whose result couldn't be read.

Two fields carry more weight than the rest:

- **`heap_min`** is the low-water mark since boot, not the sample. A
  once-a-minute reading walks straight past a transient trough, and the
  trough is what ends a long run.
- **`reason`** makes session boundaries visible in a file that many runs
  append to. Without it, a power cycle mid-drive reads as counters
  spontaneously resetting — a firmware fault, rather than someone catching
  the USB cable.
- **`sats_in_view` alongside `sats`.** The used count stays 0 for the whole
  acquisition window, so a row reading `sats=0 fix_type=1` is identical
  whether the antenna sees twelve satellites or is unplugged. In view is the
  leading indicator and the difference between "wait" and "go outside".
- **`max_flush_ms` and `max_session_ms` are separate on purpose.** The
  first is the worst detection-flush bus hold, the second the worst
  health-row hold. Both are real costs to the radio, so the worst hold
  overall is the max of the two — but only the first says anything about
  whether batch sizing needs retuning. Merged, a once-a-minute
  instrumentation write reads as evidence against the batch buffer.
- **`logger_stack_free`** is the logger task's stack high-water mark. That
  task owns the card and now has a deeper call path than it did, and a
  stack overflow there loses the whole run silently — so the size is
  reported by the run rather than argued about beforehand.
- **`gps_max_loop_gap_ms` and `gps_oversize_drops`, added 2026-08-23**,
  exist to test one specific theory about `nmea_bad_crc` rather than just
  keep restating the symptom. The GPS task is deliberately Core 0's lowest
  priority (§2); the leading theory is that a busy logger starves it long
  enough for the UART ring buffer to overflow. `gps_max_loop_gap_ms` is a
  direct measurement of that starvation — the worst gap the GPS task ever
  went without a chance to drain the buffer — rather than an inference from
  its downstream effect. `gps_oversize_drops` catches a related but
  invisible failure mode: a dropped byte that happens to be a sentence's
  own terminator merges two sentences into one, which overruns the
  96-byte assembly buffer and gets silently discarded without ever
  incrementing `nmea_bad_crc` — so the true corruption rate could be higher
  than that counter alone reports. See PROGRESS.md's `nmea_bad_crc` watch
  item for the run that motivated this.

Costs about 180 rows on a three-hour drive: nothing next to the detection
log, and one extra short bus hold a minute is far under the noise floor of
the flushes already happening.

### 8.3 How the files behave across runs

Within a run, both files are **append-only and survive power cycles.** The
header is written once, only when the file does not already exist; every
subsequent write is an append. Nothing truncates or overwrites, so a card
accumulates every run it has ever seen until someone deletes the folders.

A card **reseated mid-drive rejoins the same run** rather than splitting one
drive across two folders — the run is resolved once per power-on, not once
per mount. The gap is still recorded honestly, as `sd` going down and back
in that run's own health rows.

**Every reset claims a run, including a USB one.** Attaching a serial
monitor toggles DTR and resets the board, so bench sessions accumulate run
folders holding a single health row and no detections. That is the honest
consequence of "a run is a power-on" and is left alone rather than papered
over: such runs are trivially identifiable (`rx=0`, one row) and cost a few
hundred bytes. A Phase 6 start/stop gate would make the run boundary an
operator decision instead, at which point this stops happening.

Absolute time comes from GPS, because this board has no verified RTC. That
has one consequence worth stating plainly: **rows written before the first
fix of a run carry an empty `timestamp_utc`.** The boot row essentially
always does, since SD mounts seconds after power-on and a cold TTFF is tens
of seconds at best.

GPS time also sets the **device system clock**, once per power-on, as soon
as a sentence carries a plausible UTC date — gated on time alone, not on a
position fix, because GPS has the time long before it has a fix and waiting
would leave the files wrong for the whole acquisition window. Without it the
system clock never leaves the epoch and FAT stamps every file 1980, which
makes a card of runs impossible to order by anything but its contents.

Two limits worth knowing. Files **created** before the clock is set keep
their 1980 creation date — that includes the run directory and both CSVs,
since they are created at mount. Their modification time corrects on the
first append after the clock is set, so a run that ever saw GPS ends up with
a truthful mtime. And a run in which GPS never supplies a date keeps 1980
throughout: inherent without an RTC.

That does not lose the session, because `uptime_s` is on every row:

- **Anchor a run in absolute time** with any row that has both fields —
  wall-clock start = `timestamp_utc - uptime_s`. Every row of that run then
  resolves, including the ones written before the fix landed.
- **Separate runs within the file** by the `reason=boot` rows, and by
  `uptime_s` resetting to a small number. Without those markers a mid-drive
  power cycle would read as counters spontaneously resetting.
- **Join the two files** on uptime: a detection's `rx_uptime_ms` falls
  between two `session.csv` rows' `uptime_s`, which says what the radio,
  SD and heap were doing when that packet arrived.
- **Concatenate runs safely** using the `run` column both files carry. It is
  redundant with the directory a file sits in, right up until several runs
  are merged for analysis — at which point every row's uptime has restarted
  at zero, and without it the merged data is silently ambiguous about which
  drive a row came from.

A run in which the GPS never fixes at all has no absolute time anywhere —
inherent without an RTC, not a logging bug. It is still fully ordered and
still self-describing via uptime.

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
