# LoRaTrace RX — Roadmap

This roadmap operationalizes the build order already decided in
`docs/DESIGN.md` §9. It doesn't change any RF or architecture decision — it adds
scope boundaries, exit criteria, and an honest read on what this specific
hardware can and can't do, so "MVP-Beta" means something concrete instead
of a vibe.

## What "MVP-Beta" means here

The smallest version of this firmware that's actually useful as a field
tool: **Meshtastic War Drive, end to end.** RX locked to the LongFast (US)
channel, every detection GPS-stamped and written to SD, running
unattended on battery. That's docs/DESIGN.md §9 phases 1–2. Everything past
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
  enabled), display driver, and every task's stack.** docs/DESIGN.md's own
  estimate of 250–380KB free heap is a guess pending `ESP.getFreeHeap()`
  on real hardware — treat every RAM-hungry decision below as provisional
  until that number is in hand.
- **WiFi AP + web UI (Phase 3).** This used to sit in the "lowest priority"
  tier below, gated behind a real free-heap number under full load. That
  number now exists: `run0007`/`run0011` (session.csv) measured heap_free
  settling at ~304KB with radio + GPS + logger + display all running,
  flat with no decline across a 2.5-hour run — real headroom, not the
  provisional estimate this gate was originally waiting on. Built
  on-demand (off by default, toggled by an operator gesture) specifically
  so its RAM/CPU/RF-noise cost is never present during an actual drive
  unless asked for — see docs/history/CHANGELOG.md for the spike this was still worth
  gating behind before building the rest of the feature on top of it.
- **Display:** a naive full framebuffer at 240×135×16bpp is ~65KB — a
  meaningful bite out of a few-hundred-KB heap. Use direct-to-panel
  partial-window writes (LovyanGFX/M5GFX style) instead of buffering a
  whole frame, especially once WiFi is in the picture.
  **Superseded 2026-08-25 (Phase 6's own hardware bench pass):** real
  glass proved direct-to-panel wrong for this UI — every intermediate draw
  call is visible the instant it happens, causing real flicker/tearing a
  build report can't catch. The actual fix was an off-screen
  `Arduino_Canvas_Indexed` framebuffer, just an indexed 1-byte/pixel one
  (~32KB, since this UI only ever uses 6 colours) rather than a full RGB565
  one (~63KB) — see docs/history/CHANGELOG.md's v0.6.2 -> v0.6.3 entry for the full
  root-cause and the near-miss (an `SPIClass::beginTransaction()` deadlock)
  ruled out along the way.
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
  channel already sits inside 868–923 (docs/DESIGN.md §1). Needs an empirical
  RSSI noise-floor sweep once hardware is in hand, then document the
  sensitivity gap rather than pretend it isn't there.
- **Meshtastic's 104-slot hash space:** Phase 1–2 only lock to the
  LongFast US default (slot 20). That's most of the real-world traffic,
  but a mesh running a non-default channel name lands on a different slot
  and is invisible to `HOME_LISTEN`. `DISCOVERY_SWEEP` (Phase 8) is what
  closes that gap — until then, be precise in docs/UI that "Meshtastic
  profile" means "default channel," not "all Meshtastic traffic."

**Genuinely open — blocks real functionality, not just polish:**
- microSD bus (SPI vs SDMMC) on this board revision — docs/DESIGN.md §7,
  unresolved. Blocks finalizing the Logger task's pin/driver choice.
- Meshtastic's exact sync-word register value, and MeshCore's
  encryption/PSK model — docs/DESIGN.md §7. Detection (RSSI/SF/BW/timing) works
  without these; reliable protocol-level filtering and payload decode
  don't. Don't hardcode a guessed value for either (CLAUDE.md house rule).

**Deliberately out of scope:**
- TX/injection of any kind — permanent non-goal per CLAUDE.md house rules,
  not a phase-ordering question.

## Phases

Each phase maps 1:1 to docs/DESIGN.md §9. "Blocking unknowns" cross-reference
docs/DESIGN.md §7 items that must be resolved (or explicitly deferred) before
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
map and SPI host assignment (see docs/history/PROGRESS.md) are unverified against real
hardware and are the first suspects if the radio stays silent.
**Status:** complete, hardware-verified. See docs/STATUS.md for current
state and docs/history/CHANGELOG.md for the full session history.

### Phase 2 — `HOME_LISTEN` + task/queue architecture + GPS + SD (= MVP-Beta)
**Goal:** the smallest genuinely useful field tool.
**Deliverable:** `radio_task`, `gps_task`, `logger_task` per the proposed
layout in CLAUDE.md; FreeRTOS queue Core 1 → Core 0; GPS-stamped
detections batched to SD per the §8 log schema.
**Exit criteria:** unattended run — power on, GPS fix acquired, detections
logged with correct lat/lon, no dropped packets attributable to SD
latency, no crash from heap exhaustion over a multi-hour run.
**Blocking unknowns:** none left. microSD is confirmed on the shared SPI
bus (arbitrated by `spi_bus.h`), and GPS reached a fix on hardware
2026-08-23 — the last piece that had never been proven.
**Status:** see docs/STATUS.md for current state and
docs/history/CHANGELOG.md for the full session history.

### Phase 3 — Web Command Center (WiFi AP + web UI)
**Goal:** get data and control off the device without ejecting the SD card.
**Deliverable:** `wifi_task` — an on-demand (off by default, operator-
toggled), WPA2-protected WiFi AP hosting a single embedded web page (no
LittleFS/SPIFFS, no new `lib_deps` — built-in `WiFi.h`/`WebServer.h` only,
see docs/history/PROGRESS.md) with three tabs: a live status dashboard (the same
counters the Serial `[status]` line and `ui_task`'s pages already expose),
a run browser to download `detections.csv`/`session.csv` per run, and a
settings form that writes `/loratrace/config.txt` (`config.h`'s existing
format/validation, reused not reimplemented) — applied on next boot, not
hot-reloaded, so `radio_task`'s real-time critical section is never touched
by another task. `ui_task` gains a long-press-any-key gesture to toggle the
AP (no keymap needed, matching its existing "any key" discipline) and a
WIFI status page.
**Blocking unknowns:** none for the feature itself — pulled forward ahead
of Phase 4 at the user's request, prioritizing operator convenience over
protocol breadth. The one thing worth confirming on real hardware before
trusting this at length is the heap/counter spike described in
docs/history/PROGRESS.md: `ESP.getFreeHeap()` before/after `WiFi.softAP()` with the
full Phase 2 task set already running, and `radioCrcErrorCount()`/
`radioQueueDropCount()`/`radioBusMissCount()` staying at 0 with the AP
active — the actual go/no-go this phase used to be gated behind, now
answerable instead of estimated (see the heap numbers above).

### Phase 4 — MeshCore profile
**Deliverable:** same `HOME_LISTEN` engine, MeshCore US-narrow table
(910.525MHz/SF7/BW62.5/CR5) wired in as a second selectable profile — the
first phase where "selectable" is real, not aspirational. This is where
docs/DESIGN.md §5's keyboard-gated profile switch (operator-selected, mutually
exclusive — Meshtastic and MeshCore never listen at once) actually gets
built, deliberately deferred from Phase 3 (docs/history/CHANGELOG.md) so it's designed
and tested against a real second channel table instead of a stub.
**Blocking unknowns:** none for basic detection; MeshCore's
encryption/PSK model (§7) still blocks payload decode, not detection.
**Status:** see docs/STATUS.md for current state and
docs/history/CHANGELOG.md for the full session history.

### Phase 5 — On-device menu UI
**Deliverable:** a real menu on `ui_task`'s display, replacing the timed
hold-gestures Phase 3/4 built as a stopgap. Pulled forward ahead of
`DISCOVERY_SWEEP`/`ENERGY_SWEEP` at the user's request — same precedent as
WiFi's Phase-3 pull-forward ahead of MeshCore — because the menu/settings
framework built here is what the rest of the project gets built around, not
because it was blocking anything below it.

Scope, decided with the user up front rather than assumed: plain `,`/`.`
move a selection (no Fn chord — the Cardputer-ADV has no dedicated arrow
keys; directional intent there is Fn held + `;`/`,`/`.`/`/`, confirmed
against M5Stack's own keyboard API docs, a dedicated Cardputer-ADV keyboard
reference, and `bmorcelli/Launcher`'s own shipped interface code), Enter
selects, Backspace goes back. Toggles only this phase — profile switch
(Meshtastic/MeshCore, reusing Phase 4's `radioRequestProfileSwitch()`
unchanged) and WiFi on/off (reusing Phase 3's `wifiToggle()` unchanged) —
no on-device numeric editing of channel params; that stays on the existing
web UI Settings tab / `config.txt`. A new read-only CHANNEL status page
shows the actual active freq/SF/BW/CR/sync word, closing a real gap (that
was previously visible only over Serial or the web UI). The two old
hold-gestures are removed, not kept as parallel shortcuts, so there's
exactly one way to trigger each action.

This is what turns "decode the keyboard" from a 56-key project of its own
into a small, low-risk slice: only four specific keys need to be identified
correctly, not the whole QWERTY layout — see `src/keyboard.h` for the full
sourcing (TCA8418 raw-event encoding, the raw-value-to-physical-position
formula, and the physical-position-to-character table, each independently
cited) and docs/DESIGN.md for the citation-level writeup.

**Blocking unknowns:** none for the menu/toggle mechanism itself — every
action it triggers already exists and was already exercised in Phase 3/4.
The keyboard decode is sourced from three independent references but,
per this project's own standing rule, not yet bench-verified against real
hardware (see docs/history/PROGRESS.md's Phase 5 checklist): press each of the four keys
once and confirm the firmware recognizes exactly that key and no other.

### Phase 6 — UI architecture redesign
**Goal:** rework `ui_task` before `DISCOVERY_SWEEP` (or anything after it)
adds another ad hoc entry to a framework that was never designed to hold
more than a couple of toggles. Pulled forward ahead of `DISCOVERY_SWEEP` at
the user's request (2026-08-25) — same restructuring precedent as WiFi's
Phase-3 pull-forward and the menu UI's own Phase-5 pull-forward, both
already in this doc's history. Unlike those two, this isn't new subsystem
bring-up: it's a scoped rework of `ui_task.cpp`/`.h`, the one file the next
two phases would otherwise keep bolting onto.
**Why now, not later:** Phase 5 shipped exactly two menu items by design
(docs/history/CHANGELOG.md/docs/DESIGN.md §9) and had already grown a third — the verbose
serial-debug toggle — by the end of the same day, with no framework change
and a stale in-code comment (`ui_task.cpp`'s "the menu has exactly two
items this phase") as the only trace of the drift. `DISCOVERY_SWEEP` would
add at least a fourth item (a sweep trigger) and `ENERGY_SWEEP` a fifth
after it; fixing the shape once beats letting two more features each bolt
on their own row.
**Deliverable:**
- **Grouped menu**, replacing the flat, fixed-size list `MENU_ITEM_COUNT`
  indexes into today. Root categories (e.g. Profile, Radio Mode, System)
  open short sub-lists, same four input keys throughout (`,`/`.` move,
  Enter act, backtick/ESC back/close) — a shape borrowed structurally from
  M5PORKCHOP's (github.com/0ct0sec/M5PORKCHOP) "Sirloin-style grouped
  modal" `Menu` class (`src/ui/menu.h` in that repo: `RootItem` entries are
  either `DIRECT` actions or `GROUP`s that open a `MenuItem` sub-list) —
  reviewed for that structural idea only, not its content or aesthetic
  (see note below).
- **Toast/notice layer** for transient feedback — a toggle firing, later a
  sweep hit — that isn't tied to whichever carousel/menu row happens to be
  on screen. Real RAM cost gets measured and reported before it ships,
  same discipline as every other RAM-hungry addition this project has made
  (WiFi AP's ~55-56KB measurement is the precedent, not an estimate).
- **Redesigned status pages.** The current five (RADIO/CHANNEL/GPS/
  SYSTEM/WIFI) are single-column stacked text (`ui_task.cpp`
  `drawRadioPage()` etc.) with real unused width on a 240px panel. Group
  related values into blocks instead of one value per line. (As-proposed
  this assumed direct-to-panel partial-window writes, no framebuffer — the
  real hardware bench pass overturned that; see the Hardware feasibility
  section above and this phase's Status note below.)
- **Adopt docs/BRAND.md's on-device labels** (`Watch`/`Probe`/`Sweep` for
  HOME_LISTEN/DISCOVERY_SWEEP/ENERGY_SWEEP; plain profile names —
  `Meshtastic`/`MeshCore`/`Reticulum`/`Spectrum` — grouped under one
  "Profile" menu row, not branded per-profile names. docs/BRAND.md's Interface
  Naming table went through two revisions the same day mid-implementation:
  first a "Mesh Trace" family name over Meshtastic/MeshCore sub-profiles,
  then walked back entirely once it was clear that branding every profile
  its own "___ Trace" name made four settings on one sniffer read like
  four separate tools — "Profile" was already this doc's preferred word
  for the axis before either revision) as the strings the UI actually
  displays. Kept as a separate UI-label layer, not a rename of the
  internal identifiers: `detection.h`'s `missionProfileName()` keeps
  emitting `meshtastic`/`meshcore` into `detections.csv` exactly as every
  already-logged run expects — docs/DESIGN.md §8's own "don't concatenate runs
  across a format change without checking the header" rule applies here
  too, and there's no reason to risk it for a cosmetic rename.
- **Design reference, not a dependency:** M5PORKCHOP was cloned locally
  (read-only) and reviewed for its menu grouping, its `NoticeKind`/
  `NoticeChannel` toast abstraction (`src/ui/display.h`), and how it
  organizes a stats/status screen (`src/ui/swine_stats.h`). Its
  gamification layer (XP/levels/achievements, `src/core/xp.h`), the piglet
  mascot/mood system (`src/piglet/`), and its aesthetic and voice are
  explicitly **not** part of this redesign — docs/BRAND.md's own guardrails
  already rule out mascots, gamified framing, and anything outside the
  "field instrument, not a consumer app" positioning. Structure was worth
  learning from; the pig was not coming with it.
**Blocking unknowns:** the toast layer's actual heap cost, unmeasured
until built. Whether the ST7789V2's partial-window writes can carry the
denser block layout above without needing a framebuffer — an assumption to
confirm during implementation, not before.
**Status:** see docs/STATUS.md for current state and
docs/history/CHANGELOG.md for the full session history (canvas/framebuffer rework,
bugs found/fixed, and the still-open WiFi AP heap/counter re-measurement).

### Phase 7 — Device optimization

**Status:** complete 2026-08-27 as v0.7.0. Hardware evidence, measured
budgets, and the operator-approved same-build repetition waiver are recorded
in docs/history/PROGRESS.md.

**Goal:** turn the current memory assumptions into measured budgets before
new scan states add load to the no-PSRAM device.

**Deliverable:** internal-heap fragmentation metrics, stack high-water marks
for all five tasks, lifecycle checkpoints around the display canvas/WiFi/CSV
downloads, a repeatable real-device matrix (`docs/HARDWARE_TESTING.md`), and
measured optimizations that preserve radio/GPS/SD/UI behavior.

**Epic priority order:**

1. **P0 — Observability and baseline.** Append largest-free-block, heap block
   counts, and every task's stack watermark to `session.csv`; bracket known
   large/transient allocations in serial; run the full WiFi+canvas workload.
2. **P1 — Task stacks.** Right-size only stacks whose worst-case watermark
   proves excess headroom. Preserve at least 25% and 1KB after the change.
3. **P2 — WiFi lifecycle and requests.** Measure repeated AP cycles and CSV
   downloads; reclaim the off-state task/server cost or remove request-time
   churn only where the measurements identify a persistent cost.
4. **P3 — Canvas allocation.** Keep the verified indexed canvas unless it is
   the limiting allocation. Any tiled/partial alternative must repeat the
   full real-glass regression pass; moving it to static RAM is not a saving.
5. **P4 — Final budget.** Record normal, WiFi-on, largest-block, and per-task
   stack budgets for Phases 8/9, explicitly accept or reject the provisional
   2.5 KB transient-scan result buffer, then complete a combined-load soak.

**Exit criteria:** every `docs/HARDWARE_TESTING.md` stage passes on one identified
build; no continuing decline in current free heap or largest block after
warm-up; `queue_drop`/`row_drop`/`bus_miss` stay 0 under combined load; task
stack margins satisfy the acceptance rule; final budgets and results are
recorded in `docs/history/PROGRESS.md`.

**Scope guardrail:** Phase 7 does not add Discovery/Energy behavior and does
not optimize from compile-time RAM percentages alone. One lever changes per
measurement cycle so gains and regressions remain attributable.

### Phase 8 — `DISCOVERY_SWEEP`

**Operator label:** Probe.

**Implementation status:** bounded acquisition slice started 2026-08-27. The
source-backed, versioned fixed candidate-plan layer is documented in
`research/phase8-discovery-research.md` and implemented in
`src/discovery_plan.h`; radio-owned CAD/receive-on-hit, observation queues,
durable Probe output, and the on-device trigger are now implemented. Hardware
validation, transient mode, and deterministic cancellation/fault coverage
remain open. This is not a phase-complete release.

**Deliverable:** a bounded-duration, radio-task-owned CAD sweep of a curated
candidate list per active profile — non-default Meshtastic slots and sourced
legacy MeshCore tuples. Every complete, cancel, timeout, and failure path
restores the resolved home configuration and reports total time away from
Watch.

Packet-bearing hits keep using the existing `Detection` pipeline. CAD-only
observations use a separate fixed-size, non-blocking queue; cumulative retry,
drop, recovery, and home-away values belong in the run summary/health log,
not every observation.

Durable Probe writes to SD. An explicitly selected transient mode may run
without SD only if Phase 7 accepts a fixed result-buffer ceiling of 2.5 KB.
It reuses the live measurement buffer, retains one result, stores no raw or
historical stream, replaces the prior result on the next scan, and displays
`NOT SAVED`. If the budget fails, Probe requires SD.

**Blocking unknowns:** curated candidate lists should be weighted by
MeshMapper-observed frequencies ([[meshmapper-pipeline]], per CLAUDE.md)
where available, not scraped defaults alone. CAD `symNum` tuning (§7)
needs bench testing against Semtech AN1200.48 before trusting false-
positive/miss rates. New sweep results use the existing carousel and Probe
controls, not a reason to reopen UI architecture a second time.

Phase 8 ships with bounded, versioned built-in plans plus the existing
per-profile home override. Persistent operator-edited candidate lists are a
post-Phase-8 enhancement requiring their own bounded schema, validation,
deduplication, provenance, and device/web editing design; they do not block
Phase 8 completion.

**Exit criteria:** correct built-in LongFast CR 4/5 is host- and OTA-verified;
a known alternate transmitter is found; CAD false-positive/miss rates are
measured; 1,000 Probe cycles pass through a deterministic automated bench
mode; deterministic cancellation/fault injection covers every acquisition
state; queues and memory remain stable; every exit restores Watch.

### Phase 9 — `ENERGY_SWEEP`

**Operator label:** Sweep.

**Deliverable:** a truthful frequency-binned energy map across the supported
868–923MHz front end, followed by selective LoRa CAD only at energy peaks,
operator-selected bins, or a sparse sourced subset. This realizes Reticulum
and General Exploration without claiming that energy is LoRa or that an
off-grid CAD hit is Reticulum; those results are labeled `unknown LoRa
candidate` until stronger evidence exists.

Each bin retains only bounded streaming statistics. Durable energy peaks use
a schema that cannot be confused with packet detections. Transient mode uses
the same Phase 7-gated 2.5 KB ceiling and `NOT SAVED` behavior as Probe.

**Blocking unknowns:** 923–928MHz front-end rolloff should be
characterized (empirical RSSI floor sweep) so the UI can be honest about
reduced sensitivity in that sub-band rather than silently under-reporting.

**Exit criteria:** timing and home-away duration are measured; injected
low/mid/high carriers land in the correct bins; quiet-band behavior is
characterized with WiFi off/on; CAD never promotes energy alone to LoRa;
24 hours of repeated sweeps show bounded memory and reliable recovery.

### Phase 10 — Field Analyzer (planned; release gate provisional)

**Deliverable:** Meter, truthful frequency waterfall, bounded live Scope,
recent captures, and a passive node roster over data acquired by Watch,
Probe, and Sweep. Field Analyzer is not another mission profile and never
controls the SX1262 directly.

Scope uses an explicit bounded `SCOPE_ACQUIRE` request owned by the radio
task. It samples one displayed frequency, exposes `Watch paused`, and restores
the resolved home configuration on complete, cancel, timeout, or failure.
Ordinary analyzer page changes consume snapshots and never retune the radio.

Analyzer storage is fixed-size and reuses the existing indexed canvas. The
initial incremental ceiling is 8 KB beyond that canvas, subject to Phase 7 and
the measured Phase 8/9 costs. Waterfall plot columns use deterministic,
host-tested aggregation of real frequency bins; Scope is never presented as
a spectrum.

**Exit criteria:** bin-to-pixel and scope-source truthfulness are verified;
bounded memory and deterministic roster eviction hold; a worst-case UI/radio
run has no drops, deadlocks, or watchdog resets; outdoor readability and
minimum-brightness rendering are tested as separate conditions.

Whether Phase 10 is required for `v1.0.x`, or follows a Phase 9-based v1.0,
is deliberately decided after Phase 9 hardware evidence exists.

### Phase 11 — Cell Tower Trace (added out of sequence, 2026-09-01)

**Operator label:** Cell Trace.

Not part of the original four-profile plan (docs/DESIGN.md §3/§9) — added at
the operator's request after real wardriving runs kept picking up energy in
the 869-894MHz North American Cellular downlink band near cell towers.
Unlike WiFi's Phase-3 pull-forward or the UI's Phase-5/6 pull-forwards
(both of which *reordered* the existing sequence), this is *appended* after
Phase 10 rather than inserted into it — Phase 9/10 keep their numbers and
their own in-progress status is unaffected.

**Deliberately not a fifth mission profile.** The SX1262 only demodulates
FSK/GFSK/MSK/LoRa/OOK — it cannot decode GSM/CDMA/LTE, so there is no
protocol to detect a channel for, and no HOME_LISTEN table to give it. It is
instead a third bounded, radio-owned action alongside Probe (`DISCOVERY_SWEEP`)
and Sweep (`ENERGY_SWEEP`): retune across a curated 101-bin, 250kHz grid
covering 869-894MHz (`cell_plan.h`), sample RSSI at each bin
(`radio.getRSSI(false)`, the same primitive Sweep's Pass A uses), log every
bin — not threshold-filtered — to its own `cell.csv` (`cell_observation.h`).
No CAD is attempted (CAD is a LoRa-preamble correlator; it will never fire on
a cellular carrier) and no packet read is attempted (there is nothing to
decode). This keeps the feature honest: it is a coarse RF-presence/strength
survey ("a strong carrier sits near this frequency, here"), not a cell-tower
identifier — no cell ID, LAC/TAC, or MCC/MNC is or can be extracted.

**Deliberately isolated from `ENERGY_SWEEP`'s Pass A/B engine**
(`performEnergySweep()`), not a parameterized reuse of it: Pass A's
35.0dB noise-floor margin (`energy_observation.h`) was bench-calibrated
against a LoRa/RF-quiet environment for *sparse* peak detection, not
continuous cellular-strength carriers — reusing it here would dress up a
guess as a calibration. `performCellSweep()` (`radio_task.cpp`) is its own
function; it shares only the generic streaming-RSSI-statistics primitives
(`EnergyBinStats`/`energyBinStatsAddSample()`/`energyRssiDbmToFixed()`,
`energy_observation.h`) that Pass A itself uses, not Pass A's acquisition
loop or its calibrated threshold.

**869-894MHz** is FCC Part 22 Cellular Radiotelephone Service downlink /
3GPP Band 5 downlink — a regulatory band edge, not a carrier-specific
channel plan (no individual ARFCN/channel table is hardcoded, consistent
with CLAUDE.md's house rule against hardcoding unverified RF parameters).
It sits entirely inside the Cap LoRa-1262's tuned 868-923MHz front end
(docs/DESIGN.md §1), so unlike General Exploration's 923-928MHz top end,
there is no front-end rolloff caveat here.

**Operator surface:** a root-level "Cell Trace" menu row (`ui_menu.h`'s
`CELL_TOGGLE`), same SD-required/start/cancel shape as Probe/Sweep, with an
async completion toast. No dedicated global hotkey and no dedicated carousel
result card in this first cut — scoped out to keep the change reviewable;
Probe/Sweep's dedicated cards (`UiPage::PROBE`/`UiPage::SWEEP`) are the
template if a result card is wanted later.

**Implementation status:** code + host-native tests (`test_cell_plan`,
`test_cell_observation`, and `test_session_log`'s extended coverage) landed
2026-09-01. **Not hardware-verified** — this was implemented in a session
with no bench access to the physical device. Before trusting it in the
field: confirm a real cell-band RSSI reading actually rises during a sweep
near a known tower (vs. floor noise the whole way through), confirm the
mutual-exclusion guards against Probe/Sweep hold on real hardware, and
confirm `cell.csv`/`session.csv`'s new columns render correctly. Same
"exists but not yet proven on glass" status Phase 8 shipped with initially.

**Non-goals, same as the rest of this project:** no GSM/CDMA/LTE decode of
any kind (impossible on this hardware, not just out of scope), no per-carrier
or per-channel identification, no claim of tower triangulation from a single
reading.

## Distribution

Two install paths, both real, serving different audiences:

- **Direct flash (`pio run --target upload`)** — the dev-iteration path.
  Fastest feedback loop, direct serial access for debugging. Primary
  method through Phases 1–2 while bring-up is still being bench-verified.
- **SD-drop via [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)**
  — the preferred *end-user* install method once builds are stable.
  Matches docs/BRAND.md's "field instrument, not a laptop-tethered tool"
  positioning: swap firmware without reflashing.

This costs us nothing extra to support. Launcher installs a plain
PlatformIO app binary (`.pio/build/cardputer-adv/firmware.bin`, standard
ESP32 image starting with the `0xE9` magic byte) straight off a FAT32 SD
card — no merged image, no manifest. Launcher's own dynamic partition
manager carves out or resizes an OTA app slot to fit whatever we hand it.

**In practice, SD-drop is already the primary test path**, not just the
eventual end-user one — it's how this gets tested on hardware that's
already running a Launcher install the operator doesn't want to disturb
with a direct USB flash. CI reflects that: every merge to `main` publishes
a rolling `dev-latest` prerelease at a stable URL
(`LoRaTraceRX-dev.bin` — see Versioning below) specifically so there's
always a current build to drop on the card without waiting on a version
tag. Direct flash remains useful when iterating fast enough that even the
CI round-trip is overhead, or for the deepest debugging (upload errors,
brick recovery).

**Confirmed:** returning from a running LoRaTrace RX build back to
Launcher is a manual restart + button combo — not something our firmware
needs to implement. No return-to-launcher hook belongs in `ui_task`.
**2026-08-22, confirmed against Launcher's own source (not a guess):** the
combo is "press any key during Launcher's own ~5s post-reset boot window,
before it auto-chains back into whatever ran last" — or, to remove the
timing pressure entirely, enable Launcher's own Settings → "Boot to
Launcher" toggle, which makes it always stop at its menu on reset instead
of auto-booting the last app. See docs/history/PROGRESS.md "Open questions — Launcher
distribution" for the full read of Launcher's boot logic.

**Follows from this:** Launcher owns the flash partition table, not our
static `platformio.ini` partition CSV — that only governs direct-flash
installs. Any state we want to survive a user switching firmwares back
and forth (settings, last-used profile, calibration data) has to live on
SD, not in a custom NVS/data partition, since a custom partition isn't
guaranteed to survive a later Launcher install. This is already
docs/DESIGN.md's "SD is the datastore" philosophy — see docs/history/CHANGELOG.md — it just
now extends to config, not only detection logs.

**Binary size:** no documented hard ceiling was found for how much flash
Launcher leaves free per app on an 8MB device, especially once Launcher
itself plus other installed firmwares share the same flash (that's the
whole point of a multi-firmware launcher — see docs/history/PROGRESS.md open
questions). Rather than target an arbitrary number like "under 4MB,"
treat it as an ongoing discipline: keep the binary as lean as the feature
set allows, and measure the real number instead of assuming one.

- **Measured, not estimated:** the Phase 1 scaffold (RadioLib + IO-expander
  init, no GPS/SD/display/WiFi yet) compiles to **312KB** — 9.5% of the
  ~3.19MB app partition our own `default_8MB.csv` direct-flash scheme
  allocates (`pio run`, logged in CI now — see below). Adding SD, GPS
  parsing, and a display library will grow this, but from a 312KB
  baseline there's a lot of room before size becomes a real constraint
  either for direct flash or for coexisting with Launcher + other apps.
  **2026-08-22 update:** adding the boot-status splash's display library
  (GFX Library for Arduino) plus SD's own footprint brought this to
  **406KB** (12.2% of the same partition) — a real +94KB data point, not
  an estimate, confirming there's still plenty of headroom.
- **WiFi is the one feature with a real size lever**, same as it's the one
  feature with the real RAM lever (`lwIP` + the WiFi driver stack is
  typically the single biggest chunk of a "full" Arduino-ESP32 build). That
  go/no-go already happened (Phase 3, shipped — see CLAUDE.md's Status
  section for the real numbers this note used to be waiting on).
- `RadioLib` compiles in support for many radio families by default;
  disabling the ones we don't use (`RADIOLIB_EXCLUDE_*` build flags, only
  SX126x needed here) is a cheap, real size reduction worth doing before
  Phase 9 (`ENERGY_SWEEP`, the other RAM/flash-hungry feature after WiFi),
  not just a nice-to-have.
- CI now measures the actual `firmware.bin` size on every build (see
  below) — track it there instead of trusting an estimate.

## Versioning

Formalizes the phase-mapped table below into an actual scheme CI and bug
reports can use.

- **Format:** `vMAJOR.MINOR.PATCH` (e.g. `v0.2.1`). MAJOR.MINOR tracks the
  build-order phase reached; PATCH increments for fixes that don't add new
  phase scope.
- **Source of truth:** `src/version.h` (`FIRMWARE_VERSION`), printed on
  the boot banner (Serial) and on `ui_task`'s SYSTEM status page (on-device,
  since Phase 2). A bug report against a specific build should always be
  traceable to this string.
- **Release trigger:** pushing a `vX.Y.Z` git tag. `src/version.h` must be
  bumped to match before tagging; release CI rejects a tag/header mismatch.
- **CI:** `.github/workflows/build.yml` runs `pio run` (+ `pio test`) on
  every push/PR — catches build breaks before they land, independent of
  tagging. `.github/workflows/release.yml` runs only on a `vX.Y.Z` tag
  push: builds, renames the output to `LoRaTraceRX-<version>.bin`
  (Launcher/SD-drop-friendly naming, per its "use simple characters" SD
  guidance), and attaches it to a **draft** GitHub Release.
- **Rolling dev build, separate from the tagged scheme:** every push to
  `main` also force-moves a `dev-latest` tag and republishes a prerelease
  there with a fixed filename (`LoRaTraceRX-dev.bin`) — a stable,
  no-tagging-required download for day-to-day hardware testing. It's
  explicitly *not* versioned or draft-gated the way real releases are:
  it can be broken, it reflects whatever's on `main` at that moment, and
  its own release notes point back at the exact commit. Cutting a real
  `vX.Y.Z` tag is a separate, deliberate step for when a phase is actually
  bench-verified — see docs/STATUS.md for current status before trusting
  either.

| Version | Corresponds to |
|---|---|
| v0.1.x | Phase 1 (serial bring-up) |
| v0.2.x | Phase 2 (MVP-Beta: Meshtastic War Drive complete) |
| v0.3.x | Phase 3 (Web Command Center: WiFi AP + web UI) |
| v0.4.x | Phase 4 (MeshCore) |
| v0.5.x | Phase 5 (on-device menu UI) |
| v0.6.x | Phase 6 (UI architecture redesign) |
| v0.7.x | Phase 7 (device optimization) |
| v0.8.x | Phase 8 (discovery sweep) |
| v0.9.x | Phase 9 (energy sweep: Reticulum + General Exploration) |
| v1.0.x | promotion target after Phase 9; whether Phase 10 is required is decided from Phase 9 hardware evidence |

**Phase 11 (Cell Tower Trace) is a deliberate exception to this table.**
It landed as a PATCH bump (`v0.8.6`, not `v0.9.x`) because it is not the
*next* build-order phase — Phase 9 (`ENERGY_SWEEP`) is still in progress, and
jumping MINOR to 9 (or past it to 11) would misrepresent Phase 9/10 as
reached when they aren't. If a later phase completes Phase 9/10 first, this
table's normal MAJOR.MINOR-tracks-phase-reached rule resumes as before; Phase
11 doesn't get its own `v0.11.x` line unless a future revision of this table
decides it should.

**Renumbered 2026-08-24** (same restructuring precedent as WiFi's Phase-3
pull-forward): the on-device UI overhaul moved from a trailing "Phase 7
polish" step to Phase 5, at the user's request, pushing `DISCOVERY_SWEEP`
and `ENERGY_SWEEP` down one each. What `v1.0` means moved with it — it used
to be "Phase 7, all profiles + UI stable," and Phase 7 was `ENERGY_SWEEP`
at the time, not UI.

**Renumbered again 2026-08-25**, same precedent a second time: reviewing
Phase 5's own menu against what `DISCOVERY_SWEEP` would add to it surfaced
that the menu had already grown past its documented two-item scope (a
third, `Debug`, row landed the same day with no framework change) — see
docs/history/CHANGELOG.md for the full session. Rather than let
`DISCOVERY_SWEEP` and `ENERGY_SWEEP` each bolt on their own ad hoc entry, a
UI architecture redesign now sits at Phase 6, pushing `DISCOVERY_SWEEP` to
7 and `ENERGY_SWEEP` to 8. `v1.0`'s meaning (all four profiles + UI stable)
is unchanged by this move — it's still a placeholder for the same total
phase count, not a re-litigated decision — revisit if it stops fitting once
Phase 6/7/8 are actually in hand.

**Renumbered again 2026-08-26:** the measured ~32KB Phase-6 display canvas
now overlaps WiFi's ~55–56KB runtime cost, while only the logger task had a
stack watermark and the post-canvas combined-load gate was still open.
Device optimization therefore became Phase 7, before either new scan state.
`DISCOVERY_SWEEP` moved to Phase 8 and `ENERGY_SWEEP` to Phase 9; their scope
did not change. `v1.0` still meant all four profiles and their UI were stable
at that point.

**Phase 10 added to the plan 2026-08-26:** Field Analyzer is accepted as
planned post-Sweep scope, including bounded radio-owned Scope acquisition.
This does not silently move the release gate: after Phase 9 hardware evidence
exists, explicitly decide whether Field Analyzer is part of `v1.0.x` or the
first post-v1.0 phase.

## Non-goals

- Transmit or injection of any kind, beyond the one-time antenna-switch
  init GPIO write. Permanent, per CLAUDE.md house rules — not a phase to
  eventually reach.
- Auto-detecting mission profile. Operator-selected via keyboard, by
  design (docs/DESIGN.md §5).
- Full protocol decode/decrypt, payload display, or key handling. LoRaTrace
  remains a metadata-first passive field instrument; safe cleartext radio
  headers are the boundary.
