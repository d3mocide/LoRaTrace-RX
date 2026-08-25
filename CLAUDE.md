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

## Proposed layout

`[x]` created, `[ ]` proposed but deferred until its build-order phase
(DESIGN.md §9) — see ROADMAP.md/PROGRESS.md for why the task/queue files
aren't scaffolded yet.

```
src/
  [x] main.cpp                   # phase 2: orchestrator — boot order, then starts tasks
  [x] radio_task.cpp / .h        # HOME_LISTEN, owns SX1262, Core 1, never blocks
  [x] gps_task.cpp / .h          # NMEA parse, last-fix mutex, Core 0
  [x] logger_task.cpp / .h       # dequeue, GPS-stamp, batched SD writes, Core 0
  [x] ui_task.cpp / .h           # lifecycle + input decode + main loop; arrived phase 2, not phase 6 as originally proposed — operator asked for field-readable status before a multi-hour run without a tethered laptop (see PROGRESS.md decisions log)
  [x] ui_pages.cpp               # status-page + menu/toast drawing, split out of ui_task.cpp (2026-08-25 cleanup pass, not a phase item — see PROGRESS.md decisions log)
  [x] ui_actions.cpp             # menu-action business logic (radio/wifi/backlight/SD calls a fired menu row makes), split out of ui_task.cpp same pass
  [x] ui_task_shared.h           # private, non-public state/helpers shared only across the three files above — not part of ui_task.h's own two-function API
  [x] keyboard.h                 # TCA8418 raw-event -> KeyAction decode for 4 keys, pure/host-testable (phase 5; not in original proposal, see PROGRESS.md decisions log)
  [x] ui_menu.h                  # grouped root/group menu state machine, pure/host-testable (phase 6; not in original proposal, see PROGRESS.md decisions log)
  [x] ui_labels.h                # BRAND.md on-device label lookups, pure/host-testable (phase 6; not in original proposal, see PROGRESS.md decisions log)
  [x] wifi_task.cpp / .h         # WiFi AP + web UI, off until toggled (phase 3; not in original proposal, see PROGRESS.md decisions log)
  [x] web_assets.h               # embedded single-page web UI (phase 3)
  [x] channel_plans.h            # per-profile RF param tables (see DESIGN.md §3)
  [x] board_pins.h               # pin map + IO-expander register constants (not in original proposal, see PROGRESS.md decisions log)
  [x] version.h                  # FIRMWARE_VERSION, single source for boot banner + release tags
  [x] config.h / .cpp            # boot-time SD channel-config override + runtime write-back for wifi_task's settings page (not in original proposal, see PROGRESS.md decisions log)
  [ ] fingerprint.h              # post-hoc protocol classification (§6, phase 7+)
  --- added during phase 2, not in the original proposal ---
  [x] detection.h                # the ~36B queue record + Meshtastic header parse + §8 CSV
  [x] nmea.h                     # NMEA primitives (field split, checksum, coord conversion)
  [x] gps_parse.h                # GpsFix + gpsApplySentence(), pure so it's host-testable
  [x] spi_bus.h / .cpp           # mutex + SPIClass for the bus SD and the SX1262 SHARE
  [x] serial_lock.h / .cpp       # mutex guarding cross-core Serial writes (not in original proposal, see PROGRESS.md decisions log)
  [x] io_expander.h / .cpp       # PI4IOE5V6408 P0: antenna switch AND GPS power
  [x] gps_probe.cpp              # standalone GPS bring-up sketch ([env:gps-probe])
  [x] battery.h / .cpp           # voltage + charge % via GPIO10/ADC1, M5Unified's board_M5CardputerADV constants (phase 2)
  [x] run_log.h                  # one wardrive = one /loratrace/runNNNN/ directory; run numbering (phase 2)
  [x] session_log.h              # session.csv periodic health record — Phase 2 exit-criterion evidence (phase 2)
  --- added during phase 6, not in the original proposal ---
  [x] backlight.h / .cpp         # ST7789V2 backlight PWM (LEDC), M5GFX's non-linear duty curve for this exact pin
  [x] display_settings.h / .cpp  # brightness + idle-dim timeout, persisted to /loratrace/display.txt, separate from config.h's channel-override scope
test/
  [x] test_channel_plans/        # host-native unit tests, pio test -e native
  [x] test_detection/            # queue-record + CSV, fixtures are REAL captured packets
  [x] test_gps_parse/            # NMEA -> fix, mostly about REFUSING a bad position
  [x] test_keyboard/             # raw TCA8418 event byte -> KeyAction, allowlist-only
  [x] test_battery/              # voltage/percent conversion, pure logic
  [x] test_run_log/              # run-directory numbering
  [x] test_session_log/          # session.csv row formatting
  [x] test_ui_labels/            # ui_labels.h's uiProfileLabel() lookups (phase 6)
  [x] test_ui_menu/              # ui_menu.h's MenuState, recursive/depth-bounded (phase 6)
.github/workflows/
  [x] build.yml                  # pio run + pio test on every push/PR + rolling dev-latest release
  [x] release.yml                # vX.Y.Z tag -> draft GitHub Release with Launcher-ready .bin
sd-template/loratrace/
  [x] config.txt                 # copy-to-SD-card example for the channel config override
[x] platformio.ini
[x] DESIGN.md
[x] ROADMAP.md
[x] PROGRESS.md
```

## House rules

- **RX-only.** No transmit path beyond what antenna-switch init requires.
  Don't add TX/injection features without an explicit ask.
- **Don't hardcode sync-word values** without verifying against upstream
  firmware source first. Both are now **resolved and cited** in
  `channel_plans.h`: Meshtastic **0x2B** (meshtastic/firmware
  `src/mesh/RadioLibInterface.h`), MeshCore **0x12** (meshcore-dev/MeshCore
  `src/helpers/radiolib/CustomSX1262.h`). MeshCore's equals RadioLib's own
  default — keep `SYNC_WORD_MESHCORE` and `SYNC_WORD_RADIOLIB_DEFAULT` as
  separate constants anyway; they mean different things and only coincide
  today. Note the sync word is an RX *filter*: a wrong value means hearing
  nothing from the target protocol while still hearing unrelated traffic
  that matches — that exact bug cost several bench sessions (PROGRESS.md
  2026-08-23).
- **Don't assume MeshCore's encryption mirrors Meshtastic's default-PSK
  model** — it doesn't necessarily; MeshCore's own docs warn against this.
- No large heap buffers. Detection struct is small (~40B); flush to SD
  often rather than accumulating.
- **New operator-facing behavior gets an on-device menu toggle**, not just
  a web-UI-only setting or a silent default. Established 2026-08-25 after
  Phase 5's menu grew a third item (verbose debug) the same day it
  shipped, with no framework change to absorb it — see PROGRESS.md's
  Decisions log and ROADMAP.md's Phase 6 (UI architecture redesign).
  Doesn't apply to one-shot boot-time config (channel overrides,
  `config.txt`) — this is about anything that changes runtime behavior
  while the device is already running.
- **Bump `src/version.h` when a phase lands.** `MAJOR.MINOR` tracks the
  build-order phase (ROADMAP.md Versioning: v0.1.x = phase 1, v0.2.x =
  phase 2, ...), `PATCH` for fixes adding no phase scope. It is bumped by
  hand on purpose — it asserts "this phase is reached", and a number that
  auto-increments every build asserts nothing. `release.yml` now fails a
  tag whose version doesn't match the header, so a mismatch can't ship.
  Build *provenance* is the automated half: `scripts/build_rev.py` injects
  `FIRMWARE_BUILD_REV` (git short SHA, `-dirty` when the tree is modified)
  into every build, because a dozen `dev-latest` binaries otherwise share
  one version string and a hardware bug report can't name which one it hit.

## Status

**Phase 1 complete and hardware-verified** (2026-08-23): radio, antenna
path, SD config override, display, and protocol-correct Meshtastic RX all
confirmed on real hardware. Heap stable under load (~338KB, no leak).

**Phase 2 complete and hardware-verified** (2026-08-23): the task/queue
architecture, GPS task, and batched SD logger held up over a genuine
multi-hour unattended run (run0007, 2.5h) — every exit-criterion counter
(`crc_err`/`queue_drop`/`bus_miss`/`row_drop`) stayed at 0 the whole time,
heap flat with no leak, continuous 3D GPS fix throughout. See PROGRESS.md
for the numbers.

**Phase 3 (WiFi AP + web UI) built and hardware-verified** (2026-08-23,
reconfirmed 2026-08-24 under live traffic): pulled forward ahead of MeshCore
at the user's request — see ROADMAP.md for why. `wifi_task` is off by
default and only starts the AP on an operator gesture, specifically so it
never runs during an actual drive unless asked for. The go/no-go this phase
was gated behind — `ESP.getFreeHeap()` before/after `WiFi.softAP()` with the
full task set running, radio counters staying at 0 with the AP active — is
answered: ~55-56KB heap cost for the AP (measured twice, a day apart, same
number both times), `crc_err`/`queue_drop`/`bus_miss`/`row_drop` all stayed
at 0 with the AP active including a 2026-08-24 session with 800+ real
detections logged while it ran. See PROGRESS.md for both measurements.

**2026-08-24 addendum:** the channel-config settings page originally
shipped here had a real bug, not just a missing feature — one shared
preset applied to whichever profile was active at Save time, so saving
while on MeshCore silently corrupted what the firmware would apply as a
*Meshtastic* override on the next boot, and a profile switch back to a
profile always reverted to its hardcoded default regardless of what had
been configured for it. Fixed the same day with per-profile overrides
(`ProfileOverrides`, `channel_plans.h`) — see PROGRESS.md's Decisions log
for the full design. Still not hardware-verified, v0.5.1.

**Phase 4 (MeshCore profile) built and fully hardware-verified**
(2026-08-24): the MeshCore US-narrow table (910.525MHz/SF7/BW62.5/CR5)
already existed in `channel_plans.h` from earlier sync-word research — what
Phase 4 actually adds is DESIGN.md §5's keyboard-gated, mutually-exclusive
runtime switch between it and Meshtastic, deliberately deferred from Phase
3 (see PROGRESS.md's 2026-08-23 decisions log) so it could be built against
a real second table instead of a stub. `radio_task.cpp` gained a depth-1
`xQueueOverwrite` mailbox so `ui_task`'s keyboard poll can request a live
retune without the radio task ever blocking on it; the original ~3s-hold
trigger was later replaced by Phase 5's menu. A MeshCore detection logs
RSSI/SF/BW/timing and the profile tag but not `node_id`/`packet_id`:
MeshCore's header layout isn't reverse-engineered and its encryption/PSK
model is still open (§7), so `radio_task.cpp` never runs Meshtastic's
header parser against it. Verified against the host-native test suite (73
tests) and, as of 2026-08-24, on real hardware: live MeshCore RX at
910.525MHz/SF7/BW62.5/CR5 with plausible RSSI/SNR (a dozen+ detections,
-58 to -64dBm), and a mid-run Meshtastic<->MeshCore switch that left
`crc_err`/`queue_drop`/`bus_miss`/`row_drop` unaffected in both directions
— with a genuine Meshtastic node and relay pair heard on one side and clean
MeshCore detections on the other, in the same run. See PROGRESS.md's
Decisions log for the full readout.

**Phase 5 (on-device menu UI) built and fully hardware-verified**
(2026-08-24): pulled forward ahead of `DISCOVERY_SWEEP`/`ENERGY_SWEEP` at
the user's request — same restructuring precedent as WiFi's Phase-3
pull-forward, see ROADMAP.md. Replaces Phase 3/4's timed hold-gestures with
real keyboard-driven navigation: `keyboard.h` decodes eleven specific
TCA8418 keys — `,`/`.` move (aliased by `;`/`/`, the keyboard's own printed
Fn-arrow diamond, added after a first bench pass), Enter acts on the
highlighted menu row, the backtick/ESC key opens *and* closes the menu
(swapped in for Backspace as BACK the same bench session that found it;
gained the menu-*opening* job in a second bench-session UX fix — see
below), `1`-`5` jump straight to a numbered carousel page — no Fn chord
(the ESC/arrow additions deliberately bind the *plain* key rather than the
upstream Fn-combo, to keep this rule rather than adopt it), sourced against
three independent references since the Cardputer-ADV has no dedicated arrow
keys, see DESIGN.md §10. `ui_task` gained a two-item menu (profile switch,
WiFi toggle — both reusing existing Phase 3/4 actions unchanged) plus a new
read-only CHANNEL status page. Deliberately narrow scope, decided with the
user up front: toggles only, no on-device numeric editing of channel params
(that stays on the web UI/`config.txt`). Verified against the host-native
test suite (73 tests).

**Full hardware bench pass, 2026-08-24, all items closed.** First pass
confirmed Comma, Period, and all five digit keys working as designed, and
surfaced the ESC/arrow-alias changes above (Backspace-as-BACK felt wrong in
hand; the operator's own attempt to use the printed Fn-arrow diamond
exposed `;`/`/` weren't wired yet). A second pass the same day confirmed
ESC/backtick and Semicolon/Slash on real hardware, then surfaced one more
UX finding acted on immediately: opening the menu with Enter "feels kind of
weird." Fixed by moving that job onto ESC/backtick instead (closes the menu
as before, now also opens it from the carousel; Enter narrowed to just
firing the highlighted row, no-op in the carousel) — re-flashed and
re-confirmed on hardware same session. Menu actions (profile switch, WiFi
toggle) and the CHANNEL page's live-switch reflection are confirmed
working too. See PROGRESS.md's Decisions log for the full session,
including a live-traffic reconfirmation of Phase 3's WiFi heap/counter
numbers that fell out of the same test.

**Verbose serial debug mode** (`logger_task.cpp`, menu's third `Debug` row,
added and hardware-verified 2026-08-24): prints each detection's full
CSV-shaped row to serial as it's dequeued, off by default. Not a planned
phase item — added because Phase 4's live-RX bench check needed to see real
RSSI/SNR and nothing else surfaces per-packet detail without an SD pull or
the WiFi AP. Lives in `logger_task.cpp` rather than `radio_task.cpp` on
purpose: it prints on Core 0 after the detection has already crossed the
queue, so the radio task on Core 1 can never block on it. Shipped with a
real bug caught the same session — its first version used multiple `Serial`
calls per line and came out torn on real hardware by `main.cpp`'s Core-1
`[status]` line landing mid-sequence, the same failure mode that comment
already documents. Fixed by collapsing each line to one buffered
`Serial.write()`. Output-only by design: a serial *command* console was
floated in the same conversation and deliberately deferred — real added
surface (a parser, and what's safe to expose) on an RX-only tool, worth
deciding on its own rather than folding into a quick debug-visibility ask.

**Serial console now has a real mutex** (`serial_lock.h`/`.cpp`, added
2026-08-24, mirrors `spi_bus.h`'s `SpiBusLock` exactly): every `Serial`
print site in the firmware takes it, except `applyConfigLine()`/
`loadProfileOverridesFromSD()`/`writeFullConfig()` in `config.cpp` (all
three are only ever called once from `setup()` before any task exists —
provably single-threaded) and `radio_task.cpp` (doesn't print at all, and
must never block on non-radio I/O — keeping it that way was a design
constraint on this fix). The 2026-08-23 "one buffer, one `Serial` call"
fix turned out to be necessary but not sufficient: a 2026-08-24 hardware
session got a real WiFi client to join the AP and save Settings — the
first time that single-buffer fix was tested under real concurrent load —
and it teared anyway (`[wifi] AP started: L`, a `[config]` write missing
everything before `BW62.5`). The mutex fixed it; re-tested with the same
real-load scenario and every message came through intact, including the
exact `[config]` line that broke before. **One unexplained residual
anomaly** (a different `[config]` line lost its prefix despite the lock,
while everything around it was clean) reads as a lower-level USB-CDC
single-write truncation, not cross-task interleaving — logged as a watch
item in PROGRESS.md rather than chased further without a logic analyzer.

**2026-08-25 — Phase 5 confirmed complete; UI architecture redesign
promoted to Phase 6, `DISCOVERY_SWEEP`/`ENERGY_SWEEP` pushed to 7/8.**
Phase 5's own checklist is fully closed (PROGRESS.md) — this isn't a
reopening of unfinished work. What prompted the reorder: Phase 5's menu
was scoped to exactly two toggles and had already grown a third (verbose
debug) by the end of the same bench day, with no framework change and only
a stale in-code comment left marking the drift. Rather than let
`DISCOVERY_SWEEP` bolt on a fourth item the same way, a UI architecture
redesign now sits at Phase 6: a grouped menu (root categories opening
sub-lists, replacing the flat list), a toast/notice layer for transient
feedback, reorganized status pages (the current five are single-column
text with real unused width on the 240px panel), and adoption of
BRAND.md's on-device labels (`Watch`/`Probe`/`Sweep`, `Mesh Trace`/`Open
Trace`/etc. — the profile-naming table itself was revised again the same
day, see below) as the UI's display strings — kept separate from the
`meshtastic`/`meshcore` identifiers `detection.h` writes to
`detections.csv`, which stay unchanged for log-format stability.
M5PORKCHOP (github.com/0ct0sec/M5PORKCHOP, cloned read-only for reference)
was reviewed for its grouped-menu and toast-notice structure — not its
gamification layer, mascot, or aesthetic, all of which BRAND.md's
guardrails already rule out. See ROADMAP.md's Phase 6 entry and
PROGRESS.md's Decisions log for the full session.

**Phase 6 (UI architecture redesign) built, not yet hardware-verified**
(2026-08-25, first pass v0.6.0): `ui_task.cpp` rebuilt on top of two new
pure/host-testable headers, `ui_menu.h` (`MenuState` — the root/group
grouped menu described above) and `ui_labels.h` (BRAND.md's profile/mode
display strings). A toast overlay confirms whatever just fired; RADIO/
CHANNEL/GPS/SYSTEM/WIFI all gained a second, right-hand column using width
the old single-column layout left blank. Verified with `pio test -e
native` and `pio run -e cardputer-adv`, both actually run rather than
assumed, but **not yet bench-tested against the real ST7789V2 panel** —
the layout's exact pixel positions were sourced from arithmetic against
the 240x135 panel, and a compiling build says nothing about whether they
clip or overlap on real glass.

**Phase 6 fully re-implemented the same day (v0.6.1)** after the operator
reviewed a pixel-accurate HTML/Canvas mockup of the actual `ui_task.cpp`
draw calls (~9 rounds of concrete visual feedback against screenshots) and
green-lit it — a substantially larger change than v0.6.0's first pass, not
a fix: every page was redrawn, not just the menu. Mid-review the user
identified a modeling problem — "Mesh Trace is a mode, the profile
selector is a sub of mesh trace" — confirmed as "Mesh Trace becomes a
[family]... we select what profile we are sniffing," which is now
BRAND.md's Interface Naming table (own "Revised 2026-08-25" note there):
Meshtastic and MeshCore collapse into one family name ("Mesh Trace"),
"Core Trace" is retired, Open Trace/Spectrum Trace are unaffected. Real
changes: `BRAND.md` (table + revision note), `ui_labels.h` (flat
`uiProfileLabel()` -> `uiTraceModeLabel()`/`uiSubProfileLabel()`/
`uiActiveProfileLabel()`), `ui_menu.h`'s `MenuAction` enum
(`PROFILE_SWITCH` -> `SELECT_MESHTASTIC`/`SELECT_MESHCORE` — both root
rows are GROUP now, "Mesh Trace" opens onto its two sub-profiles instead
of cycling one at a time), `channel_plans.h`'s `nextHomeListenProfile()`
deleted as dead code, and a full `ui_task.cpp`/`.h` port: header status
dots (GPS fix, heap health, RX activity — replacing the old idle
heartbeat blink outright), profile/page-position moved into a new footer
status line, a redesigned toast (flush-bottom slide-in band with a
countdown bar, driven by a bounded ~60ms fast-redraw burst for its own
~1.4s lifetime rather than a continuous loop), CHANNEL's frequency-position
bar (868–923MHz, the SX1262's real tuned range, DESIGN.md §1) plus a rough
time-on-air estimate, GPS's constellation counts as 4 bars instead of a
text line, and WIFI folded into SYSTEM's now-2x2 grid with a heap-fraction
bar (`UiPage::COUNT` is 4, not 5). Two things had no real-hardware
equivalent and were adapted rather than copied literally: the mockup's
alpha-blended RX-pulse decay became a binary hold-then-revert (RGB565/
Arduino_GFX has no cheap alpha blending), while the toast's slide/
countdown-bar animation needed no alpha at all — just per-frame rectangle
geometry.

Host-native tests: `test_ui_menu` grew from 11 to 13 (Mesh Trace group
coverage), `test_ui_labels` now 7 (family/sub-profile split),
`test_channel_plans` lost the deleted function's test — **92/92 `pio test
-e native` passing**, and `pio run -e cardputer-adv` **SUCCESS** (static
RAM 50304/327680B, flash 957753/3342336B per the build report), both
actually run rather than assumed. **Still not bench-tested against the
real ST7789V2 panel** — same bar every other visual change in this project
has shipped at before its own bench pass, and now a bigger check than the
v0.6.0 pass covered since every page changed (see PROGRESS.md's Phase 6
checklist). The design mockup itself was preserved as a living reference
per the operator's request, to build on for Phase 7/8's menu additions
without a fresh mockup built from scratch each time:
https://claude.ai/code/artifact/84eb5187-9f26-4fc1-8b6b-39f9969a86ea

**v0.6.1's "Mesh Trace" branding walked back the same day (v0.6.2).** The
operator reviewed the shipped naming and pushed back: branding
Meshtastic/MeshCore as a "Mesh Trace" family — and Reticulum/General
Exploration as "Open Trace"/"Spectrum Trace" — made four settings on one
sniffer read like four separate products, and overloaded "Trace" three
ways (the product name, a per-profile brand, and a saved-session noun).
**Profile** replaces all four "___ Trace" names — not a new word, it was
already this doc's own preferred term ("'Profile' instead of 'attack
mode'") before the Trace-branding detour. Presets keep their real names:
**Meshtastic**, **MeshCore**, **Reticulum**, **Spectrum** (short for
General Exploration). **Trace goes back to meaning exactly one thing: a
saved session or run.** `ui_task.cpp`'s root menu barely changes shape —
still two groups, the label just changes from "Mesh Trace" to "Profile" —
and `ui_labels.h` collapses back to one flat `uiProfileLabel()`, since
there's no branded family left to compose a family/sub-profile string
from. `test_ui_labels` shrinks from 7 tests to 3 (one flat lookup +
collision check instead of a two-tier one); `test_ui_menu` stays at 13,
just its fixture's root label renamed. **92/92 -> 88/88 `pio test -e
native` passing**, `pio run -e cardputer-adv` **SUCCESS** (RAM
50304/327680B, flash 957645/3342336B) — both actually run. The design
mockup was corrected and republished again at the same link above, its
before/after section's genuinely historical parts (Phase 5's frozen
record) left untouched as always.

**v0.6.2 -> v0.6.3: Phase 6's first real hardware bench pass** (2026-08-25
evening). The redesign compiled and matched its mockup, but direct-to-panel
drawing has failure modes a build report can't catch. Full-screen blink/
flicker on every redraw and tearing during the toast animation, both real:
root cause was `drawPage()` unconditionally wiping the entire content region
on *every* redraw (idle 1Hz tick and the 60ms animation burst included), plus
a fully redundant `fillScreen()` in `nextPage()`/`prevPage()`/`jumpToPage()`
on top of that. First-pass fixes (drop the redundant clear, give the toast a
lightweight header-only fast path) reduced but didn't eliminate it — every
draw call is visible on the glass the instant it happens with no buffering.
Real fix: an off-screen `Arduino_Canvas_Indexed` (`ui_task.cpp`'s `tft` now
points at it, not the panel) — every draw call targets this buffer, nothing
reaches the panel until one `tft->flush()` blits the whole frame in a single
SPI burst (`Arduino_TFT::drawIndexedBitmap`, confirmed against the vendored
GFX source). `_Indexed` since this UI only ever uses 6 colours: ~32KB instead
of RGB565's ~63KB, a real one-time `malloc()` given no PSRAM (DESIGN.md §1),
not a static allocation. This reverses the "no canvas/framebuffer,
direct-to-panel" choice v0.6.1 documented — a deliberate, considered reversal
once real hardware found a problem that choice had no answer for, not a
casual one. (A near-miss: an early draft wrapped draws in an outer
`tft->startWrite()`/`endWrite()` pair for batching; reading the vendored
`Arduino_HWSPI`/`SPIClass` source first showed this deadlocks on the first
nested `fillRect()` call, since `SPIClass::beginTransaction()` takes a plain
non-recursive lock. Never reached hardware.)

Same bench pass, smaller fixes: the menu's root-row values were overflowing
the panel width by exactly the wrap boundary (`"Profile Meshtastic >"` =
240px at size-2 text) — fixed by dropping the decorative `"> "` from every
root row rather than shrinking text, landing on `"Profile: Meshtastic"`
(228px). The WiFi menu toggle's toast was reporting the *opposite* of what
had just happened (toasted "WiFi OFF" on the request that turned it on) —
`fireMenuAction()` was reading `wifiIsEnabled()` *after* `wifiToggle()`,
which only queues a request `wifiTask` (Core 0) hasn't applied yet; fixed by
reading the pre-toggle state and negating it, the pattern the profile-switch
toast already used correctly.

**Trace pause/standby** (new, same session): a real battery lever for the
radio-listening + logging pipeline, added after investigating whether GPS or
the radio task should be gateable for heap/battery reasons. GPS turned out
not to be independently power-gateable at all — `io_expander.h`'s P0 line
does both antenna-switch enable and GPS power, so a GPS-off toggle would
deafen the radio too — and neither GPS's nor the radio task's stack (3072B/
4096B) is a meaningful heap lever next to WiFi's AP (~55-56KB) or this same
session's new canvas (~32KB). What's real: `radio_task.cpp` can now put the
SX1262 into `radio.sleep(true)` (warm sleep, retains config) instead of
continuous RX, via a second one-slot mailbox mirroring the existing
profile-switch pattern; resuming is a plain `radio.startReceive()` with no
re-`begin()`. GPS is deliberately left running throughout — no power benefit
to pausing it, and position stays fresh for the instant Trace resumes. Shows
as a root-level menu row (`"Trace: Active"`/`"Trace: Standby"`, promoted out
of the System group the same session on operator feedback that it's central
enough to fire directly) and a `STANDBY` banner on RADIO's page. Named
"Trace" after an explicit naming check — "MeshTrace" was rejected outright
(revives the branding this doc's own v0.6.2 entry walked back), and plain
"Trace" was flagged too since BRAND.md pins that word to exactly one meaning
(a saved session/run); kept anyway as a documented, narrow exception — the
saved-session noun is always countable, the live-toggle usage always pairs
with a state word, so the two don't collide in practice (BRAND.md's
Interface Naming section now says so explicitly). Confirmed on real
hardware: pausing/resuming, and switching profiles *while* paused correctly
wakes and retunes the radio.

Two more from the same pass: the SYSTEM page's heap bar filled with *free*
space and emptied toward the danger zone — backwards next to a colour that
was getting more alarming as the bar got shorter. Now fills with *used*
heap against the ~512KB no-PSRAM budget, colour and direction escalating
together, with a real red tier at 90% used (was a flat 2-tier threshold
before) — one shared `heapUsageColour()` now drives the bar, the "k heap"
text, and the header's heap dot. And the web dashboard had the same
ambiguity RADIO's new STANDBY banner just fixed on-device: `/api/status`
had no field saying whether the radio was actually listening. Added
`trace_paused` to the JSON and a status card on the dashboard.

While already in `wifi_task.cpp`: `handleRuns()`'s JSON was built with
`String` concatenation in a loop (`json += String(idx)` per run directory)
— real reallocation/copy churn on every dashboard poll, worse as field
sessions accumulate. Replaced with a fixed stack buffer + `snprintf`,
matching every other handler in this file; `streamCsvFile()`'s
`Content-Disposition` header (three chained `String` allocations for one
request) got the same treatment.

`pio test -e native` **88/88** (same total as before this session — Trace's
test coverage moved from a System group item to a root DIRECT row, it
didn't add a net-new test), `pio run -e cardputer-adv` **SUCCESS** (RAM
50312/327680B, flash 961325/3342336B). Bench-verified at multiple points
through the session on the attached Cardputer, not just at the end. See
PROGRESS.md's Decisions log for the full session, including the exact
mailbox/wake-sequence details for the radio sleep path.

**v0.6.3 -> v0.6.4: real backlight brightness control, shipped the same
evening.** The v0.6.3 Brightness menu (4 fixed presets, plain digitalWrite
backlight) worked fine as an on/off toggle but had no real dimming behind
it. Building it out into an actual PWM-driven control surfaced a genuine
hardware bug, not just missing polish: a naive LEDC setup (20kHz, then
1kHz, linear 0-100% duty mapping) blacked the display out at various
brightness levels instead of dimming it, non-monotonically enough (50%/75%
fixed by dropping to 1kHz; 25% then broke at 1kHz right after) that it
didn't fit a simple "wrong frequency" story. Root cause found by reading
M5Stack's own board-support code for this exact pin: `M5GFX.cpp`'s
`board_M5CardputerADV` autodetect branch drives GPIO 38 (`PIN_TFT_BL`) at
**256Hz** through LovyanGFX's `Light_PWM::setBrightness()` — a **non-linear
curve with a built-in minimum-duty floor** (`offset=16`), not a naive
linear map. `backlight.cpp` now replicates that exact formula and those
exact constants rather than guessing again. All 4 original presets
confirmed working afterward; the low end (5-20%) is new range this session
opened up and needs its own bench sweep before being fully trusted, same
"verify, don't assume" discipline as everything else physical here.

Two operator-requested upgrades landed the same evening, once the PWM was
trustworthy:
- **Brightness became a real slider** (5-100% in 5% steps, PREV/NEXT to
  scrub, live-applied every step) instead of 4 fixed points, and **idle-dim
  timeout became configurable** (Off/30s/60s/2min/5min, cycled from a menu
  row) instead of a hardcoded 60s. Both now **persist to SD**
  (`/loratrace/display.txt`, `display_settings.h`/`.cpp` — a new, narrowly
  scoped module mirroring `config.h`'s load/write pattern on purpose rather
  than growing that file past the "channel overrides only" scope it
  documents for itself) — the device remembers what was chosen across power
  cycles now, the same way channel overrides already did. This is
  `ui_task`'s first-ever SD access; it arbitrates `spi_bus.h`'s mutex the
  same bounded (2s) way `writeProfileConfigToSD()` already does for the web
  UI's own settings save. The idle-dim floor is now `min(15%, whatever the
  operator's active level is)` rather than a fixed 15% — needed once
  brightness could go below that, so idle-dimming can't make the screen
  brighter than a deliberately low active level.
- **The menu gained real nesting**: "System > Display > Brightness/Idle
  dim" replaces Brightness living at root and Idle-dim living flat in
  System, on direct operator request. This needed more than a table
  reshuffle — `ui_menu.h`'s `MenuState` was an explicitly two-level design
  ("root, then one group inside it, never more," stated since the Phase 6
  redesign), and a GROUP nested inside a GROUP is a genuine third level.
  Rather than special-case just this one screen, `MenuState` was
  generalized into a small recursive/stack-based model (`MenuItem` rows —
  ACTION/GROUP/SLIDER — bounded to `MAX_DEPTH = 4`, only 3 actually used
  today) — the second time tonight a nesting need came up (System's own
  flat WiFi/Debug/Trace grouping was the first), which is what made
  generalizing rather than special-casing again the right call. Every
  list-drawing function in `ui_task.cpp` collapsed from three (root/group/
  slider-specific) to one generic `drawMenuList()` plus `drawMenuSlider()`
  as a side effect — less code, not more, once the data model stopped
  hardcoding depth. The header breadcrumb now shows the full path (e.g.
  "MENU > System > Display > Brightness").

`pio test -e native` **90/90**, `pio run -e cardputer-adv` **SUCCESS** (RAM
50328/327680B, flash 964425/3342336B). Confirmed on the attached Cardputer
through an extended live debugging session (a background serial monitor
watching `[backlight]` diagnostic lines while brightness levels were
selected in real time) before the M5GFX root cause was found, then
reconfirmed after — not just built and assumed. See PROGRESS.md's
Decisions log for the full session, including the exact duty-curve math and
the specific 20kHz/1kHz dead ends ruled out along the way.

**2026-08-25 (later still) — docs/code cleanup pass, requested by the
operator ahead of Phase 7/8.** A survey found `ui_task.cpp` at 1265 lines
mixing six largely-independent concerns (page drawing, menu/toast
rendering, keyboard-to-action decode, cross-task business logic, animation
timing, canvas lifecycle) — the only real "monolith" signal in the tree.
Split into `ui_task.cpp` (lifecycle/input/main loop), `ui_pages.cpp`
(drawing), `ui_actions.cpp` (menu-action business logic), and a new
private `ui_task_shared.h` for the state genuinely shared across the
three — `ui_task.h`'s own two-function public API is unchanged. Caught two
real bugs along the way rather than shipping them: a `#include
"detection.h"` initially flagged as dead actually wasn't (a direct call to
`detectionFormatTimestamp()` a narrower grep pattern missed; a passing
build had only masked it via a transitive include through `radio_task.h`),
and the split itself introduced a real linker collision (`ui_task.cpp`'s
internal `tft` pointer, once given external linkage to share across the
new files, collided with `main.cpp`'s own pre-existing global `tft` for
the boot splash) — caught immediately by `ld`, fixed by renaming ui_task's
copy to `uiTft`. Also fixed a stale `"phase 5"` string hardcoded into the
boot banner/splash (the device was on Phase 6/v0.6.4) and closed out an
already-resolved `TODO(verify)` on the display pins. `pio run -e
cardputer-adv` **SUCCESS** — RAM byte-identical to the pre-split baseline
(50328B), flash +180B (expected cross-translation-unit cost). `pio test -e
native` **90/90**, unchanged. A one-off `-Wall -Wextra` pass found no new
warnings anywhere in the split. **Not yet bench-tested against real
hardware** — a real compile/link/test pass is stronger evidence than this
project's usual "reviewed by inspection" fallback, but this is still the
display/input subsystem, and a compiling build has never been proof glass
renders correctly here. See PROGRESS.md's Decisions log and Phase 6
checklist for the full session.

**2026-08-25 (later still) — boot mark: BRAND.md's unbuilt logo concept
built.** The plain-text "LoRaTrace RX" / version splash lines are now a
procedural boot-mark animation — an L-shaped path resolving into three
signal arcs (BRAND.md's own long-unbuilt concept), sequential arc reveal
with an amber "lock" flash on each (chosen over a simultaneous "radar
ping" take via a mocked-up preview, artifact link in PROGRESS.md), then
the wordmark and version line. The diagnostic boot log underneath is
unchanged in content, just repositioned to align under the mark's first
arc per direct feedback on the mockup. Drawn with `Arduino_GFX`'s
`fillArc()`/`drawArc()` (first use of either anywhere in this firmware —
angle convention verified against the vendored source, not assumed: 0° is
12 o'clock, clockwise), not a bitmap. `pio run -e cardputer-adv`
**SUCCESS** (+6.0KB flash, +4B RAM — the flash number is real cost
verified by measurement, not the smaller estimate first assumed), `pio
test -e native` **90/90** unaffected. **Not yet bench-tested on real
hardware** — direct-to-panel drawing on the one part of this project that
has never gotten a layout fully right without a real bench pass. See
PROGRESS.md's Decisions log for the mockup-review process and the real
engineering deltas (geometry rescaled, angle convention, flash cost)
found only by actually building it.

Three hard-won rules from Phases 1–2, worth not relearning:
- **The IO expander's P0 powers the GPS as well as switching the RF antenna
  path.** A "dead" GPS or a silent radio is often just this.
- **A wrong sync word is silent, not loud** — the radio simply never
  interrupts, while still hearing unrelated traffic that matches.
- **A same-node-id detection pair with wildly different RSSI seconds apart
  is very likely a genuine mesh relay, not a logging bug** — `packet_id`/
  `hop_limit`/`hop_start`/`relay_node` in `detections.csv` (added after
  run0007) prove it either way; don't assume duplicate-detection without
  checking those first.

Build order is DESIGN.md §9. See PROGRESS.md for the live build checklist
and ROADMAP.md for phase-by-phase scope.

## Related context

[[meshmapper-pipeline]] already holds real-world MeshCore frequency
observations for this area — check it before trusting a scraped "US
default" number for the channel tables.
