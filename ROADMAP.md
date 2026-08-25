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
- **WiFi AP + web UI (Phase 3).** This used to sit in the "lowest priority"
  tier below, gated behind a real free-heap number under full load. That
  number now exists: `run0007`/`run0011` (session.csv) measured heap_free
  settling at ~304KB with radio + GPS + logger + display all running,
  flat with no decline across a 2.5-hour run — real headroom, not the
  provisional estimate this gate was originally waiting on. Built
  on-demand (off by default, toggled by an operator gesture) specifically
  so its RAM/CPU/RF-noise cost is never present during an actual drive
  unless asked for — see PROGRESS.md for the spike this was still worth
  gating behind before building the rest of the feature on top of it.
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
  and is invisible to `HOME_LISTEN`. `DISCOVERY_SWEEP` (phase 7) is what
  closes that gap — until then, be precise in docs/UI that "Meshtastic
  profile" means "default channel," not "all Meshtastic traffic."

**Genuinely open — blocks real functionality, not just polish:**
- microSD bus (SPI vs SDMMC) on this board revision — DESIGN.md §7,
  unresolved. Blocks finalizing the Logger task's pin/driver choice.
- Meshtastic's exact sync-word register value, and MeshCore's
  encryption/PSK model — DESIGN.md §7. Detection (RSSI/SF/BW/timing) works
  without these; reliable protocol-level filtering and payload decode
  don't. Don't hardcode a guessed value for either (CLAUDE.md house rule).

**Deliberately out of scope:**
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
**Status:** **complete, hardware-verified 2026-08-23.** Both suspects above
were real: expander P0 turned out to power the GPS as well as the antenna
switch, and the sync word had to be sourced from upstream rather than
guessed. Live Meshtastic frames decoded with well-formed headers, and heap
stayed flat under sustained RX.

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
**Status:** built; **the exit criterion is now the only thing outstanding.**
Every component works on hardware individually. What has not happened is
the multi-hour unattended run itself, which is the whole point of the
criterion — an architecture that survives a bench session and one that
survives three hours of real traffic on battery are different claims.
`session.csv` (DESIGN.md §8.2) exists so that run can be judged from the
card afterwards instead of requiring someone to watch a console.

### Phase 3 — Web Command Center (WiFi AP + web UI)
**Goal:** get data and control off the device without ejecting the SD card.
**Deliverable:** `wifi_task` — an on-demand (off by default, operator-
toggled), WPA2-protected WiFi AP hosting a single embedded web page (no
LittleFS/SPIFFS, no new `lib_deps` — built-in `WiFi.h`/`WebServer.h` only,
see PROGRESS.md) with three tabs: a live status dashboard (the same
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
PROGRESS.md: `ESP.getFreeHeap()` before/after `WiFi.softAP()` with the
full Phase 2 task set already running, and `radioCrcErrorCount()`/
`radioQueueDropCount()`/`radioBusMissCount()` staying at 0 with the AP
active — the actual go/no-go this phase used to be gated behind, now
answerable instead of estimated (see the heap numbers above).

### Phase 4 — MeshCore profile
**Deliverable:** same `HOME_LISTEN` engine, MeshCore US-narrow table
(910.525MHz/SF7/BW62.5/CR5) wired in as a second selectable profile — the
first phase where "selectable" is real, not aspirational. This is where
DESIGN.md §5's keyboard-gated profile switch (operator-selected, mutually
exclusive — Meshtastic and MeshCore never listen at once) actually gets
built, deliberately deferred from Phase 3 (PROGRESS.md) so it's designed
and tested against a real second channel table instead of a stub.
**Blocking unknowns:** none for basic detection; MeshCore's
encryption/PSK model (§7) still blocks payload decode, not detection.
**Status:** built, not yet hardware-verified. The MeshCore table itself
predates this phase (channel_plans.h, sourced and cited during Phase 1's
sync-word investigation); what this phase adds is the actual live switch —
`radio_task.cpp` holds a depth-1 mailbox (`xQueueOverwrite` + a task notify)
so a switch request from `ui_task` retunes the SX1262 between packets
without the radio task ever blocking, and `ui_task`'s gesture state machine
gained a third, ~3s-hold bucket alongside its existing tap/~1.2s-hold pair —
still no keymap needed, same discipline as the Phase 3 WiFi toggle. See
CLAUDE.md's Status section and PROGRESS.md for what's verified so far
(host-native tests only) versus what a hardware session still needs to
confirm (live MeshCore RX, and that a mid-run switch leaves the radio
counters clean afterward).

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
cited) and DESIGN.md for the citation-level writeup.

**Blocking unknowns:** none for the menu/toggle mechanism itself — every
action it triggers already exists and was already exercised in Phase 3/4.
The keyboard decode is sourced from three independent references but,
per this project's own standing rule, not yet bench-verified against real
hardware (see PROGRESS.md's Phase 5 checklist): press each of the four keys
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
(PROGRESS.md/DESIGN.md §9) and had already grown a third — the verbose
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
  related values into blocks instead of one value per line — still
  direct-to-panel partial-window writes, no framebuffer (this doc's
  existing ~65KB framebuffer caution still applies), just organized better.
- **Adopt BRAND.md's on-device labels** (`Watch`/`Probe`/`Sweep` for
  HOME_LISTEN/DISCOVERY_SWEEP/ENERGY_SWEEP; plain profile names —
  `Meshtastic`/`MeshCore`/`Reticulum`/`Spectrum` — grouped under one
  "Profile" menu row, not branded per-profile names. BRAND.md's Interface
  Naming table went through two revisions the same day mid-implementation:
  first a "Mesh Trace" family name over Meshtastic/MeshCore sub-profiles,
  then walked back entirely once it was clear that branding every profile
  its own "___ Trace" name made four settings on one sniffer read like
  four separate tools — "Profile" was already this doc's preferred word
  for the axis before either revision) as the strings the UI actually
  displays. Kept as a separate UI-label layer, not a rename of the
  internal identifiers: `detection.h`'s `missionProfileName()` keeps
  emitting `meshtastic`/`meshcore` into `detections.csv` exactly as every
  already-logged run expects — DESIGN.md §8's own "don't concatenate runs
  across a format change without checking the header" rule applies here
  too, and there's no reason to risk it for a cosmetic rename.
- **Design reference, not a dependency:** M5PORKCHOP was cloned locally
  (read-only) and reviewed for its menu grouping, its `NoticeKind`/
  `NoticeChannel` toast abstraction (`src/ui/display.h`), and how it
  organizes a stats/status screen (`src/ui/swine_stats.h`). Its
  gamification layer (XP/levels/achievements, `src/core/xp.h`), the piglet
  mascot/mood system (`src/piglet/`), and its aesthetic and voice are
  explicitly **not** part of this redesign — BRAND.md's own guardrails
  already rule out mascots, gamified framing, and anything outside the
  "field instrument, not a consumer app" positioning. Structure was worth
  learning from; the pig was not coming with it.
**Blocking unknowns:** the toast layer's actual heap cost, unmeasured
until built. Whether the ST7789V2's partial-window writes can carry the
denser block layout above without needing a framebuffer — an assumption to
confirm during implementation, not before.

### Phase 7 — `DISCOVERY_SWEEP`
**Deliverable:** bounded-duration CAD-cycle sweep of a curated candidate
list per active profile — non-default Meshtastic slots, legacy MeshCore.
**Blocking unknowns:** curated candidate lists should be weighted by
MeshMapper-observed frequencies ([[meshmapper-pipeline]], per CLAUDE.md)
where available, not scraped defaults alone. CAD `symNum` tuning (§7)
needs bench testing against Semtech AN1200.48 before trusting false-
positive/miss rates. New sweep results are additional entries in Phase 6's
grouped menu, not a reason to reopen UI architecture a second time.

### Phase 8 — `ENERGY_SWEEP`
**Deliverable:** Reticulum and General Exploration profiles — FSK/OOK RSSI
sweep across 868–923MHz with periodic LoRa CAD checks at common SF/BW
combos.
**Blocking unknowns:** 923–928MHz front-end rolloff should be
characterized (empirical RSSI floor sweep) so the UI can be honest about
reduced sensitivity in that sub-band rather than silently under-reporting.

## Distribution

Two install paths, both real, serving different audiences:

- **Direct flash (`pio run --target upload`)** — the dev-iteration path.
  Fastest feedback loop, direct serial access for debugging. Primary
  method through Phases 1–2 while bring-up is still being bench-verified.
- **SD-drop via [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)**
  — the preferred *end-user* install method once builds are stable.
  Matches BRAND.md's "field instrument, not a laptop-tethered tool"
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
of auto-booting the last app. See PROGRESS.md "Open questions — Launcher
distribution" for the full read of Launcher's boot logic.

**Follows from this:** Launcher owns the flash partition table, not our
static `platformio.ini` partition CSV — that only governs direct-flash
installs. Any state we want to survive a user switching firmwares back
and forth (settings, last-used profile, calibration data) has to live on
SD, not in a custom NVS/data partition, since a custom partition isn't
guaranteed to survive a later Launcher install. This is already
DESIGN.md's "SD is the datastore" philosophy — see PROGRESS.md decisions
log — it just now extends to config, not only detection logs.

**Binary size:** no documented hard ceiling was found for how much flash
Launcher leaves free per app on an 8MB device, especially once Launcher
itself plus other installed firmwares share the same flash (that's the
whole point of a multi-firmware launcher — see PROGRESS.md open
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
  Phase 8 (`ENERGY_SWEEP`, the other RAM/flash-hungry feature after WiFi),
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
- **Release trigger:** pushing a `vX.Y.Z` git tag. `src/version.h` should
  be bumped to match *before* tagging — CI doesn't currently cross-check
  the two, so a mismatch is a review-time catch, not an automated one.
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
  bench-verified — see PROGRESS.md for current status before trusting
  either.

| Version | Corresponds to |
|---|---|
| v0.1.x | Phase 1 (serial bring-up) |
| v0.2.x | Phase 2 (MVP-Beta: Meshtastic War Drive complete) |
| v0.3.x | Phase 3 (Web Command Center: WiFi AP + web UI) |
| v0.4.x | Phase 4 (MeshCore) |
| v0.5.x | Phase 5 (on-device menu UI) |
| v0.6.x | Phase 6 (UI architecture redesign) |
| v0.7.x | Phase 7 (discovery sweep) |
| v0.8.x | Phase 8 (energy sweep: Reticulum + General Exploration) |
| v1.0.x | all four profiles built, on-device UI covers all of them |

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
PROGRESS.md's Decisions log for the full session. Rather than let
`DISCOVERY_SWEEP` and `ENERGY_SWEEP` each bolt on their own ad hoc entry, a
UI architecture redesign now sits at Phase 6, pushing `DISCOVERY_SWEEP` to
7 and `ENERGY_SWEEP` to 8. `v1.0`'s meaning (all four profiles + UI stable)
is unchanged by this move — it's still a placeholder for the same total
phase count, not a re-litigated decision — revisit if it stops fitting once
Phase 6/7/8 are actually in hand.

## Non-goals

- Transmit or injection of any kind, beyond the one-time antenna-switch
  init GPIO write. Permanent, per CLAUDE.md house rules — not a phase to
  eventually reach.
- Auto-detecting mission profile. Operator-selected via keyboard, by
  design (DESIGN.md §5).
- Full protocol decode/decrypt as an MVP-Beta requirement — depends on the
  two genuinely-open unknowns above (sync word, MeshCore PSK model) and
  isn't needed for the core "detect and log" value proposition.
