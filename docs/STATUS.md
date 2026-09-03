# Status

The one place "where is this project right now" lives. Replaces status
prose that used to be duplicated (and drifting) across `CLAUDE.md`,
`PROGRESS.md`, and `README.md`. For *how* we got here, see
`docs/history/CHANGELOG.md`; for the phase-by-phase build order, see
[ROADMAP.md](ROADMAP.md).

## Current version

**v1.0.0** (`src/version.h`). `MAJOR.MINOR` tracks the build-order phase
*reached*, not the phase in progress — see ROADMAP.md's Versioning
section. Phase 9 (`ENERGY_SWEEP`/"Sweep") reached 2026-09-03: all five
ROADMAP.md exit criteria closed, including two full 8-hour endurance
soaks that caught and fixed a real `logger_task` stack overflow and a
proactive `radio_task` stack margin fix — see "What's hardware-verified"
below for the full writeup. `v0.8.6`-`v0.8.9` were all PATCH bumps for
out-of-sequence Phase 9/11 additions landed while Phase 9 itself was
still open (Cell's repeat mode, FCC A/B block markers, the Sweep region
setting) — see ROADMAP.md's Versioning section for why those stayed
PATCH rather than MINOR. **Phase 10 (Field Analyzer) reached MINOR status
2026-09-03 and closed all five exit criteria the same day** — see the
Phase 10 entry below. `v0.10.1` is a same-day PATCH fixing a real bug the
worst-case run itself surfaced (a repeat-mode Waterfall race condition);
`v0.10.2`/`v0.10.3` are same-day PATCHes for real, hardware-confirmed UI
polish (Waterfall's frequency axis + repeat toggle, Meter's bar gauge/SNR/
channel-param block) — see the Phase 10 entry's "Post-closure UI polish"
paragraph below. Same PATCH-not-MINOR convention as the Phase 9/11 bumps
above throughout. **Promoted to `v1.0.0` the same day**: ROADMAP.md's own
documented gate for this promotion was Phase 10 closing, and only that —
that's done. Phase 11 (Cell) was never part of the gate (added out of
sequence, outside the original four-profile scope); its own two open
items (below) are known, tracked gaps post-`v1.0`, an explicit operator
call, not an oversight.

## What's hardware-verified

Phases 0-9 are complete and hardware-verified: radio bring-up (Phase 1),
the task/queue architecture + GPS + SD logging that makes up MVP-Beta
(Phase 2), the WiFi AP + web command center (Phase 3), the MeshCore
profile and live profile switch (Phase 4), the on-device menu UI
(Phase 5), the UI architecture redesign (Phase 6), measured heap/stack
budgets and soak (Phase 7 — its strict same-build repetition criterion
was explicitly waived for that cycle, see `docs/history/PROGRESS.md`),
bounded radio-owned discovery scanning with a source-backed candidate
plan (Phase 8, `DISCOVERY_SWEEP` / "Probe"), and frequency-binned energy
acquisition with selective Pass-B CAD (Phase 9, `ENERGY_SWEEP` /
"Sweep", reached 2026-09-03 — full writeup below). Phase 8's statistical
CAD false/miss matrix remains an explicit lab follow-up — the available
bench can't provide a known-quiet RF control.

**Phase 9 (`ENERGY_SWEEP` / "Sweep") is complete — all five exit
criteria closed:**
- Pass A (frequency-binned energy acquisition across 868-923MHz) is
  hardware-verified end-to-end: real `energy.csv` peak rows spanning the
  full band, zero queue drops, clean home-restore across repeated sweeps.
- Serial Control (`SWEEP_START`/`SWEEP_CANCEL`/`STATUS`) and a dedicated
  on-device Sweep result card are wired and hardware-verified.
- The noise-floor margin is calibrated from a bench-run matrix: the
  shipped default moved from a 10.0dB placeholder to a measured 35.0dB,
  re-verified at 0/221 false peaks across three consecutive production
  sweeps.
- **Region setting (`v0.8.9`, added and hardware-verified 2026-09-01):**
  System > Region narrows Sweep's scanned band to 902-923MHz (US, the
  default) instead of the full 868-923MHz range (Global), roughly
  halving scan time — 47 CFR § 15.247 cited in `energy_plan.h`.
  Persisted to `/loratrace/region.txt`. Cell and `channel_plans.h` are
  explicitly NOT region-aware (see `docs/ROADMAP.md`'s Phase 9 follow-up
  bullets). Confirmed on real hardware (`b7c845a`): System > Region
  shows US by default and cycles to Global and back; a US-region Sweep
  shows the frequency bar reading 902/923 and completes noticeably
  faster; a Global-region Sweep reads 868/923 at full duration, matching
  pre-change behavior; the choice survives a reboot. One bug was caught
  and fixed during this verification: the System menu's row count was
  still hardcoded to 3 after Region became its 4th row, silently hiding
  the new row — `ui_task.cpp`'s `SYSTEM_GROUP_ITEMS` GROUP entry now
  correctly says 4.
- **Pass B (CAD at peaks) is implemented and hardware-verified** (landed
  `8367b73`/`5dae6ab`, 2026-08-28/29 — this bullet was previously stale and
  said "has not started"). CAD runs immediately at each Pass-A peak, capped
  at `PASS_B_MAX_PEAKS_PER_SWEEP` (8) peaks per sweep, across the 10-row
  sourced SF/BW table in `pass_b_plan.h`. A CAD hit that promotes to a real
  packet logs as a `Detection` with `off_grid = true`
  (`detectionClassification()` returns `unknown_lora_candidate`, never a
  mission-profile name — the DESIGN.md §7.2 requirement that Pass B must
  not mislabel an off-grid hit as Reticulum). Per-combo confidence
  (`PassBConfidence`: `STRONG`/`NOISY`/`UNVERIFIED`) is a descriptive
  `energy.csv` column derived from a pooled 1,200-cycle bench matrix across
  three physical setups (open room, both radios shielded, Cardputer-only
  shielded) — only SF8/BW125 (`STRONG`, 2/60 quiet FP, 20/20 real-pulse
  detection) and SF11/BW500 (`NOISY`, 22/60 quiet FP, 19-20/20 detection)
  are reproducible enough to carry a confidence tag; the other eight combos
  stay `UNVERIFIED` pending more bench cycles. Full history, the SF-vs-time
  confound investigation, and the shielded-box findings are in
  `docs/research/phase9-sweep-pass-b-design.md`.
- **923MHz-edge front-end rolloff — closed, no rolloff found (2026-09-01).**
  ROADMAP.md's blocking unknown is resolved. Two lines of evidence, both
  real hardware:
  - **Passive floor pass** (`BENCH_SWEEP_FLOOR`, `scripts/phase9_rolloff_bench.py`):
    three real GLOBAL-band sweeps compared the mean floor in the top 5MHz
    near the front end's 923MHz ceiling against the rest of the band —
    +0.26dB, +0.52dB, -0.06dB, all inside the mid-band's own bin-to-bin
    spread. No signature, but passive (no transmission), so it couldn't
    rule out reduced *gain* on a real signal.
  - **Injected-carrier pass** (`BENCH_RSSI_WINDOW`, `scripts/phase9_edge_carrier_bench.py`):
    parks the radio at one fixed frequency and samples RSSI continuously
    for ~2s (a full Sweep's per-bin dwell, tens of ms, is too short to
    reliably coincide with an independently-timed transmitter's burst — a
    first attempt using a full Sweep came back a meaningless null result
    for exactly that reason). With the Heltec and Cardputer on matched
    915MHz whip antennas and physically separated (desktop/under-desk,
    ruling out the near-field USB/clock coupling the Pass B shielded-box
    study already found raises apparent noise between close-together
    radios), 26 combined trials across two sessions at a mid-band
    (912.8125MHz, `LONG_MODERATE`) and an edge-band (920.625MHz,
    `SHORT_SLOW`) sourced candidate: mid-band captured the real signal
    12/14 tries at -34.7dBm average, edge-band **14/14** at -36.9dBm
    average — only a 2.3dB gap, and the edge band was if anything *more*
    reliable, not less. An earlier close-together run showed a much
    bigger apparent gap (~25dB) that turned out to be a measurement
    artifact: computing signal "rise" as pulse-RSSI-minus-quiet-RSSI
    silently absorbs whatever the quiet baseline itself is doing, and the
    quiet baseline at 920.625MHz measured a real, reproducible ~24dB
    higher than at 912.8125MHz in this room with nothing transmitting —
    a genuine RF-environment fact (this is a dense urban area with AMI
    smart-meter deployments, which commonly use FHSS in the 900-928MHz
    band, a very plausible source), not a receiver characteristic. The
    fix was comparing absolute captured-signal strength instead of a
    delta from a baseline that isn't equal between the two frequencies
    being compared.
- **RTL-SDR ground truth for Pass B's CAD-at-arbitrary-bin question
  (2026-09-02).** `bench/rtl-sdr/sync_cad_capture.py` triggers
  `BENCH_PASS_B_CAD` and a synchronized RTL-SDR capture at the same
  918.5MHz test point, reading the raw result back via a new
  `BENCH_PASS_B_CAD_RESULT` opcode. All 10 `PASS_B_SF_BW_CANDIDATES`
  combos, quiet condition, n=5 each: every `CAD_DETECTED` (6 total
  instances, concentrated in SF11/BW500 as expected) showed a completely
  flat SDR waterfall at 918.5MHz — direct, not statistical, confirmation
  of the symbol-duration false-trigger hypothesis
  (`docs/research/phase9-sweep-pass-b-design.md`). A positive-control
  pass (`--pulse`, arming the Heltec once at 0ms delay immediately before
  each trigger — the timing this script's first, unsuccessful attempt
  got wrong) matched the original bench matrix exactly: the exact-match
  combo and the three whose bandwidth is a superset of the injected
  125kHz signal all hit 5/5, confirmed independently on the SDR
  waterfall; everything else 0/5.
- **Three more Phase 9 exit criteria closed (2026-09-02):**
  - *Timing and home-away duration measured.* Three real US-region
    sweeps: device-measured away time (`EA`, a new `STATUS` field
    exposing `radioEnergyLastAwayMs()`, previously internal-only)
    3386-3438ms, home channel correctly restored every time.
  - *Quiet-band behavior characterized with WiFi off/on.* Three matched
    off/on pairs (`WIFI_SET`, a new Serial Control opcode mirroring the
    on-device menu toggle): zero peaks in every sweep regardless, and no
    meaningful timing difference (EA within ~50ms either way). WiFi does
    not introduce false Sweep peaks or measurably change sweep duration.
  - *CAD never promotes energy alone to LoRa.* 10 real `BENCH_PASS_B_CAD`
    attempts at the noisiest known combo (SF11/BW500): 5 came back
    `CAD_DETECTED`, and the real-packet-promotion counter (`PBD`) never
    moved once, across any attempt. Empirical confirmation, not just the
    code-level guarantee (`off_grid` is only ever set after a real
    decoded packet).
  - *Injected low/mid/high carriers land in the correct bins.*
    `scripts/phase9_bin_accuracy_bench.py` — the dedicated test
    `research/LoRaTrace-Phases-7-10-Design.md`'s hardware matrix
    specifies ("Sweep calibration... known signals at low/mid/high
    bins"), separate from Endurance. Real sourced candidates at the low/
    mid/high ends of the US region (`LONG_SLOW` 905.3125MHz,
    `LONG_MODERATE` 912.8125MHz, `SHORT_SLOW` 920.625MHz), each fired
    repeatedly (0ms-delay ARM, ~10Hz) through a live Sweep's whole
    duration to beat the ~40ms-per-bin dwell window: 11/24 attempts
    landed a real elevated `BENCH_SWEEP_FLOOR` reading, and all 11 were
    at the exact pre-computed bin index for that frequency — never a
    neighbor, never wrong. Sub-100% hit rate is expected (same dwell-vs-
    pulse-timing reality the 923MHz-edge work established), not a
    correctness gap; what mattered was 11/11 correct-bin attribution.
  - *Endurance soak, scoped to 8 hours — found and fixed a real crash
    bug.* No cited technical derivation exists anywhere in this
    project's docs for 24 hours specifically — it's a round-number
    target in the original design table
    (`research/LoRaTrace-Phases-7-10-Design.md`'s hardware matrix), and
    8 hours of back-to-back laps is already several thousand cycles,
    well past where a real leak or stability bug would be expected to
    surface. Documented as a deliberate, reasoned deviation, same
    convention Phase 7's own soak criterion was relaxed under once
    (`docs/history/PROGRESS.md`). `scripts/phase9_soak.py`, production
    firmware, WiFi off then on partway through (matching the design
    table's own "Off, then On" row).

    **First 8-hour run:** 4,143/4,148 laps completed, 5 failures. This
    was *initially* written up here as "0.1%, the project's own already-
    documented native-USB dropped-response pattern, not a device fault"
    — **that was wrong**, caught only by pulling `session.csv` off the SD
    card afterward and cross-referencing run-directory boundaries against
    the soak's own timeline. All 5 "failures" were actually **identical,
    100%-reproducible hard crashes**:
    ```
    Guru Meditation Error: Core 0 panic'ed (Unhandled debug exception).
    Debug exception reason: Stack canary watchpoint triggered (logger)
    ```
    same backtrace every time, `logger_task`'s own stack watermark
    plunging from 952B free at boot to 84B free before the first one.
    Each crash triggered a real `RTC_SW_CPU_RST` (software reset) and
    silently rebooted the device — which also fully explains the
    previously "unresolved" WiFi anomaly from the same run: WiFi wasn't
    buggy, the whole device rebooted and came back up in its normal
    boot-default (off) state, no code-path mystery required. Root cause:
    `logger_task`'s 5,120-byte stack (`logger_task.cpp`, sized by
    inspection when added, never load-tested until this soak) was
    genuinely undersized for `writeSessionRow()`'s frame depth (a
    ~50-field `SessionStats` struct + a 320-byte row buffer, calling into
    SD/FatFS from the bottom of it).

    **Fix:** bumped to 8,192 bytes, matching `wifi_task`'s own stack (a
    comparably deep SD/network call path) rather than guessing at
    another inspection-based number.

    **3-hour verification re-run, same day:** 1,507 laps, only 1 failure
    (a genuine isolated dropped response this time, not a crash — the
    device answered normally again the very next poll), **zero**
    `Guru Meditation`/`RTC_SW_CPU_RST` events, and WiFi stayed on
    continuously for 1,007 straight laps after being switched on with
    no reversion. Covers the timing of the first two original crashes
    (which hit at 0.34h and 2.81h into the original run) with margin.
    Not a full 8-hour re-confirmation, but real, clean, contradicting
    evidence against the bug recurring.

    **Second full 8-hour run, with the logger fix in place:** 4,353/4,353
    laps, 0 failures, 0.0%. Zero `Guru Meditation`/`RTC_SW_CPU_RST`
    events anywhere in the log. One continuous run directory on the SD
    card (`run0143`) spanning the entire 8 hours confirms no reboot of
    any kind occurred, not just no crash. WiFi, switched on at the
    4-hour mark, stayed on for all 2,340 remaining laps with zero
    reversions.

    **"Bounded memory" directly confirmed** from `run0143`'s real
    `session.csv`: `heap_free`/`heap_largest`/`heap_allocated_blocks` all
    show a clean step function, not a decline — flat for the ~3.7 hours
    before WiFi turned on, one legitimate ~56KB one-time drop exactly
    when the AP started (its real allocation cost, matching Phase 7's own
    "recovered transient allocation, not a leak" distinction), then flat
    again for the remaining 4+ hours with WiFi running continuously. No
    drift in either phase.

    **Timing tail, fully explained, not a bug.** The recurring 19-35s
    laps (147/4,143 in the first run, a similar count in the second) are
    100% explained: every single `wp=0` (no peak found) lap took ≤3.5s;
    every single `wp>0` lap took ≥4.2s, scaling with peak count (`wp=2`
    laps cluster at 15-35s). This is Pass B correctly doing its
    documented job — up to 10 SF/BW combos' worth of CAD, plus a bounded
    2.5s receive-on-hit window per combo that detects, for every real
    peak Sweep finds — not an anomaly. About 5% of sweeps in this room
    found something worth investigating; those sweeps take proportionally
    longer by design.

    **`radio_task` stack margin, found and fixed the same way:** pulling
    `run0143`'s real numbers (not just logger's) showed `radio_stack_free`
    settling at a lifetime-minimum of 820B free out of 4,096 allocated
    (20.0%) within the first two hours and holding flat there for the
    rest of the run — not a leak, but below this project's own margin
    rule (25% or 1KB, whichever is larger). `radio_task` is the single
    most critical task in the system (owns the SX1262, must never
    block), so this was bumped proactively (4,096 → 6,144, proportionate
    to logger's own fix) rather than left at a margin already under the
    house rule just because it hadn't overflowed yet. A focused 2-hour
    verification run afterward — chosen to cover the ~1.9h mark where the
    old watermark hit its floor, with margin — came back 1,011/1,011
    laps, 0 failures, 0 crashes, WiFi stable throughout.

    **All five Phase 9 exit criteria are now closed**, 2026-09-03.

Phase 10 (Field Analyzer) is accepted as planned scope. Whether it's
required before `v1.0.x` was an explicit decision deferred until Phase 9
hardware evidence exists (ROADMAP.md) — that evidence exists now (above),
and **the decision was made 2026-09-03: Phase 10 is required for `v1.0.x`.**
Work is starting under an interim `v0.10.x` line, the same convention
Phase 8/9 used while in progress; see ROADMAP.md's Phase 10 entry and
Versioning table.

**Phase 10 (Field Analyzer) — `v0.10.1`, all five exit criteria closed
2026-09-03:**
- **Data layer, `SCOPE_ACQUIRE`, and the on-device UI (Meter/Waterfall/
  Scope/Captures/Nodes) are done and hardware-verified**, including a
  second hub — **Tools**, gating Probe/Sweep/Cell — added at the operator's
  request the same session (real scope beyond ROADMAP.md's own Phase 10
  text, not a deviation from it). Two real bugs were found and fixed on
  real hardware during this pass: a whole-`WaterfallHistory` snapshot
  (~5.5KB) overflowing `ui_task`'s 4096B stack (crashed the device opening
  Analyze > Waterfall), and the carousel-position footer reading straight
  off the raw `UiPage` enum ordinal instead of the main-carousel-relative
  index (would have shown "14/14" instead of "2/6"/"3/6" for the two new
  hub cards). Full writeup: ROADMAP.md's Phase 10 entry.
- **Memory budget — closed, confirmed two ways.** `ANALYZER_STATIC_BYTES`
  (compile-time `sizeof()`, `analyzer_budget.h`) measures the four
  analyzer structures at **6,728 of the 8,192-byte incremental ceiling**
  (82.1%, 1,464B headroom). A real 29-minute `session.csv` (`run0006`,
  below) confirms it end-to-end on hardware, not just at compile time.
- **Worst-case UI/radio run — closed.** WiFi on, Sweep repeat mode
  running continuously, Waterfall open, for a full 60 real minutes. A
  background Serial Control watch (231 `STATUS` polls at 15s intervals)
  recorded zero dropped/unanswered requests and zero `task_wdt`/`Guru
  Meditation` signatures for the entire hour.
- **Outdoor and minimum-brightness readability — closed** (operator
  check, 2026-09-03): confirmed good both in direct window sunlight and
  indoors.
- **A real bug the worst-case run itself surfaced — found and fixed
  same day, `v0.10.1`.** Pass A found 50 energy peaks over that hour
  (`STATUS`'s `PBA=50`), yet Waterfall showed nothing the whole time.
  Root cause: `analyzerNoteSweepComplete()` (`analyzer_state.cpp`, Core 0)
  read `radio_task.cpp`'s live per-sweep peak-bin mask, but in repeat mode
  `radio_task`'s own do-while loop calls straight back into
  `performEnergySweep()` for the next lap with no delay, and that lap's
  first line resets the same mask — Core 0's ~100ms poll cadence almost
  always lost that race, so every repeat-mode Waterfall row read an
  already-cleared mask regardless of what Pass A actually found.
  `energy.csv` itself was never affected (a separate, queue-based path
  that logs each peak the instant Pass A finds it). **Fix:** a second,
  stable snapshot buffer (`energyPeakBinMaskAtComplete`) taken atomically
  at sweep completion, read via a new `radioEnergyPeakBinSetAtLastComplete()`
  accessor; the Sweep page's own live occupancy ticks
  (`drawSweepOccupancy()`) are untouched, still reading the live mask on
  purpose so they keep updating progressively during an active sweep.
  **Hardware-confirmed same day**: operator re-ran Waterfall during a live
  repeat Sweep against real MeshCore traffic and confirmed hits now
  appear on the display.

**Post-closure UI polish, same day (`v0.10.2`/`v0.10.3`), hardware-
confirmed** — real scope beyond the five exit criteria above, not a
deviation: Waterfall gained a frequency axis (later merged into the plot
box's own bottom border to remove a redundant line) and an Enter key that
starts/stops repeat Sweep straight from the page. Meter gained a real bar
gauge, an SNR line, and a right-column SF/BW/CR block — all real
`CaptureSummary` data this page had access to and never showed — plus a
range widened -30 -> 0dBm after a real -16dBm reading clipped flat
against the original ceiling. Full writeup: `docs/ROADMAP.md`'s Phase 10
entry.

**Hardware finding, 2026-09-03 — found, isolated, and resolved by an SD
card swap.** While attempting the Stage 4 hardware verification above,
every cold boot hit a 100%-reproducible `task_wdt` abort ~13-14s after
`[config] Applied channel override(s)` — `logger` (Core 0) ran long
enough during SD bring-up to starve `IDLE0` past the watchdog's 5s
window, hard-resetting the device into a boot loop, with
`[W] sd_diskio.cpp:180 sdCommand(): crc error` firing immediately before
it every time. Isolated by flashing the last tagged release
(`v0.9.0`/`b84d88c`, no Phase 10 code) to the same device: identical
crash, timing, and SD CRC warning — confirming this was the SD card, not
firmware, in either version. **Operator swapped the card same day; the
same `v0.10.0` build now boots cleanly** — GPS clock sync, radio task
start, and a Serial Control `HELLO`/`STATUS` round-trip all confirmed
(`STATUS` reports `SD=1`), zero `task_wdt`/`Guru Meditation` signatures
across a reset and a subsequent **20-minute passive Serial Control watch**
(40 `STATUS` polls at 30s intervals, 16:55-17:15 UTC, every poll
answered, `SD=1` throughout, no reboot). Confirms the fix holds under a
sustained idle run, not just a single clean boot.

**`session.csv` pulled off the card afterward (operator reattached it
directly, 2026-09-03) — real confirmation, not just the compile-time
number.** `run0006` covers a real 29-minute boot (`uptime_s` 3 through
1744, boot row through 29 periodic rows at the correct 60s cadence):
`analyzer_static_bytes,6728` on every single row, `sd=ok` throughout,
zero `row_drop`/`queue_drop`/`bus_miss`/`crc_err` for the entire run, and
`heap_free`/`heap_min` settling from 258,488B at boot to ~211,552B/
207,108B within the first ~10 minutes and holding perfectly flat for the
remaining ~19 — the same "one legitimate one-time settle, then flat"
signature Phase 9's own soak established as healthy, not a leak. This
closes the memory/telemetry side of Stage 4 for real, not just on paper.

**Phase 11 (Cell) — added out of sequence, PARTIALLY hardware-verified:**
a bounded RSSI-only presence sweep of 869-894MHz (North American Cellular
downlink), operator-requested after real wardriving runs picked up energy
in that band near cell towers. It is not a decode of any kind — the SX1262
cannot demodulate GSM/CDMA/LTE — and not a fifth mission profile; same
operator surface as Probe/Sweep (global hotkey C, dedicated carousel card),
not a menu row. See `docs/ROADMAP.md`'s Phase 11 entry and `docs/DESIGN.md`
§5a for the full design and why it's numbered outside the normal phase
sequence. Code and host-native tests landed 2026-09-01; on 2026-09-01 it
was flashed to real hardware (v0.8.6, `b7c845a`) and confirmed: the C key
runs a one-shot Cell scan end-to-end (toast through `Cell: DONE` with a
real MHz/dBm reading on the carousel card, home channel restored after),
and the Probe/Sweep mutual-exclusion guard holds in both directions
(Cell during an active Sweep gives `Sweep` priority and refuses with
`Cell: UNAVAILABLE`; Sweep during an active Cell scan is refused the same
way). Still unverified: a real sweep near a known tower showing RSSI
rising above the floor, and `cell.csv`/`session.csv`'s new columns written
correctly to SD.

**Repeat mode (R), `v0.8.7`, hardware-verified 2026-09-01:** Cell gained a
repeat mode identical in shape to Sweep's own (`radioRequestCellSweepRepeat()`,
back-to-back laps with a lap counter on the card). This also changed
Sweep's own R key: it was a global hotkey (fired from anywhere, including
with the menu open); it's now page-gated by `ui_task.cpp` to the Sweep/Cell
cards specifically, matching how Enter (`SELECT`) already dispatches
per-page — a no-op on any other page or with the menu open. Probe
deliberately has no repeat mode (operator decision: "Repeat only on the
Sweeps"). Confirmed on real hardware (`b7c845a`, same session as Cell's own
verification above): Sweep-repeat still starts/stops correctly after the
page-gating change; Cell-repeat starts/stops correctly with its lap counter
rendering cleanly; R is a no-op on Probe (including mid-scan), every other
page, and with the menu open from either Sweep or Cell; and mutual
exclusion holds in both directions across a real repeat chain, not just a
single shot (Cell refused during Sweep-repeat, Sweep refused during
Cell-repeat).

**FCC A/B block markers, `v0.8.8`, hardware-verified 2026-09-01:** the Cell
frequency bar now labels the FCC's own downlink sub-band split within
869-894MHz — Block A (869-880MHz + 890-891.5MHz) and Block B (880-890MHz +
891.5-894MHz), cited to 47 CFR § 22.905 (`cell_plan.h`'s
`CELL_BAND_BLOCKS`) — as a two-shade tick row under the bar with letter
labels on the two segments wide enough to hold one. Regulatory block letter
only, deliberately no carrier name (current licensee varies by market and
isn't a fixed national fact — see `docs/DESIGN.md` §5a). Confirmed on real
hardware (`b7c845a`): the tick row renders cleanly with no overlap against
the lo/hi labels above or the disclaimer line below.

## What's still open

- ~~Bench SD card / boot-loop finding~~ — resolved 2026-09-03. The
  bench Cardputer's `task_wdt` boot-loop (see Phase 10 section above) was
  the SD card, not firmware; operator swapped it same day, and a clean
  boot plus a 20-minute zero-crash passive Serial Control watch confirmed
  the fix. Bench device trusted again.
- ~~Phase 10 (Field Analyzer) hardware confirmation~~ — closed
  2026-09-03. All five exit criteria done, including a real bug (a
  repeat-mode Waterfall race condition) the worst-case run itself
  surfaced and got fixed the same day; see the Phase 10 section above for
  the full writeup.
- **Sweep silence near real MeshCore traffic — diagnosed, 2026-09-03: it's
  dwell timing, not the noise floor.** Originally opened as "is 35.0dB too
  conservative?" after a worst-case run found 50 Pass-A peaks but zero
  packet promotions while Watch/Trace saw plenty of real traffic from a
  6-foot-away MeshCore repeater (operator's own `pyMC_Repeater` node, real
  local mesh). Three independent lines of evidence closed this out:
  - **The floor itself is fine.** The repeater's own noise-floor monitor
    reports -94.0dBm average at this exact location; its packets average
    -34.8dBm — a ~59dB clearance over the floor, well past the 35dB
    margin. Real traffic here is nowhere near the threshold.
  - **Watch/Trace has no reception bug.** A live correlation (Serial
    Control's new `RXP`/`RXC` fields, `radioPacketCount()`/
    `radioCrcErrorCount()`, added this session) logged 96 real packets in
    6 minutes with zero CRC errors — more than the repeater's own log
    showed for the same window, since Trace hears the whole broadcast
    mesh, not just one node's vantage point. (An earlier reading of this
    session mistakenly used `R`, which is Probe's own recovery counter,
    not a packet count despite the letter — corrected before drawing any
    conclusion from it.)
  - **Direct RTL-SDR ground truth confirms the dwell-timing theory.** A
    focused 909-912MHz capture during 19 live single-shot Sweep laps
    showed near-continuous real bursts at 910.5MHz (roughly one every
    1-8 seconds, 182 flagged events over ~5 minutes) — yet Sweep only
    registered a peak on 5/19 laps (26%), even with transmissions that
    dense nearby. A ~40ms dwell at one bin per lap only rarely coincides
    with an independently-timed burst; this is a receive-window problem,
    not a sensitivity problem. Matches (with much denser real traffic)
    the same effect the 923MHz-edge bench work already established for a
    single controlled signal.
  - **Bonus, unplanned finding:** the same test showed Trace decoding only
    6 packets in ~5 minutes while Sweep laps ran back-to-back, versus 96
    in 6 minutes with the radio otherwise free — a real, now-quantified
    ~15x drop in Watch/Trace's catch rate while Sweep (repeat or
    back-to-back single-shot) monopolizes the radio. Not a bug, an
    inherent trade-off worth knowing about, not previously measured.
  
  No firmware change indicated by this investigation — the margin, the
  reception path, and the dwell design are all working as built. Making
  the margin operator-configurable (floated earlier) is no longer
  motivated by a suspected miscalibration, though it could still be
  useful as a general tuning knob if a future need justifies the menu-
  toggle work CLAUDE.md requires for it. `RXP`/`RXC` stay on `STATUS`
  going forward — genuinely useful for future "is Trace actually
  receiving" questions, not just this one.
- **Cell hardware verification** (Phase 11, above) — C key, mutual
  exclusion against Probe/Sweep (both directions), and the carousel card
  are now confirmed on real hardware (2026-09-01). Still open: confirm a
  real cell-band RSSI reading actually rises near a known tower, and
  confirm `cell.csv`/`session.csv`'s new columns write correctly to SD.
- ~~Phase 9's endurance soak~~ — closed 2026-09-03. All five exit
  criteria are done; see the Phase 9 section above for the full soak
  writeup (a real crash bug found and fixed along the way).
- Pass B's other eight SF/BW combos still have only n=20/condition
  (`UNVERIFIED`) — more bench cycles would be needed before extending
  `STRONG`/`NOISY` past the two combos that have it now
  (`docs/research/phase9-sweep-pass-b-design.md`'s open questions).

`docs/history/PROGRESS.md` has a much older "Open questions" list dating
back to Phases 1-2 (2026-08-22 through 2026-08-27). Most of those are
resolved and marked `[x]` there; a few unresolved ones (MeshCore's
encryption/PSK scheme, CAD `symNum` tuning, Meshtastic's exact per-slot US
frequency table) were never revisited after that file stopped being the
live status doc — treat them as leads to re-check, not confirmed-current
open items.

## Three hard-won hardware facts

Kept here as well as in `CLAUDE.md` (agents read that file, humans are
more likely to land here first):

- **The IO expander's P0 powers the GPS as well as switching the RF
  antenna path.** A "dead" GPS or a silent radio is often just this.
- **A wrong sync word is silent, not loud** — the radio simply never
  interrupts, while still hearing unrelated traffic that matches.
- **A same-node-id detection pair with wildly different RSSI seconds
  apart is very likely a genuine mesh relay, not a logging bug** —
  `packet_id`/`hop_limit`/`hop_start`/`relay_node` in `detections.csv`
  prove it either way.
