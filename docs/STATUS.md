# Status

The one place "where is this project right now" lives. Replaces status
prose that used to be duplicated (and drifting) across `CLAUDE.md`,
`PROGRESS.md`, and `README.md`. For *how* we got here, see
`docs/history/CHANGELOG.md`; for the phase-by-phase build order, see
[ROADMAP.md](ROADMAP.md).

## Current version

**v0.8.9** (`src/version.h`). `MAJOR.MINOR` tracks the build-order phase
*reached*, not the phase in progress — see ROADMAP.md's Versioning
section. `v0.8.x` = Phase 8 complete; Phase 9 is underway, so the version
correctly hasn't moved to `0.9.0` yet. The PATCH bump is Phase 11 (Cell,
below) — an out-of-sequence addition, not a fix, but not the
next build-order phase either; see ROADMAP.md's Versioning section for why
that's a PATCH bump rather than a MINOR one. `v0.8.7` adds Cell's repeat
mode and page-gates Sweep's own R key (below) — still Phase 11 scope, so
another PATCH, not a MINOR. `v0.8.8` adds FCC A/B block markers to the
Cell frequency bar (below) — same reasoning, another PATCH. `v0.8.9` adds
a Sweep region setting (below, Phase 9 scope this time, not Phase 11) —
same out-of-sequence-addition reasoning, another PATCH.

## What's hardware-verified

Phases 0-8 are complete and hardware-verified: radio bring-up (Phase 1),
the task/queue architecture + GPS + SD logging that makes up MVP-Beta
(Phase 2), the WiFi AP + web command center (Phase 3), the MeshCore
profile and live profile switch (Phase 4), the on-device menu UI
(Phase 5), the UI architecture redesign (Phase 6), measured heap/stack
budgets and soak (Phase 7 — its strict same-build repetition criterion
was explicitly waived for that cycle, see `docs/history/PROGRESS.md`),
and bounded radio-owned discovery scanning with a source-backed candidate
plan (Phase 8, `DISCOVERY_SWEEP` / "Probe"). Phase 8's statistical CAD
false/miss matrix remains an explicit lab follow-up — the available bench
can't provide a known-quiet RF control.

**Phase 9 (`ENERGY_SWEEP` / "Sweep") is in progress:**
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

    **Timing tail, separately** (not related to the crash): the first
    8-hour run also showed 147/4,143 laps (~3.5%) taking 19-24s instead
    of the ~3.4s median, scattered evenly across the run, every one
    still completing and restoring home correctly. Not yet root-caused;
    a plausible candidate is periodic SD/GPS activity briefly contending
    for the shared SPI bus (`spi_bus.h`), but that's a hypothesis, not
    confirmed, and wasn't re-checked in the shorter verification run.

    **"Bounded memory" still not directly confirmed** via `session.csv`'s
    real heap numbers from a long run post-fix — the indirect timing
    signal is good, but a full-length re-soak with the fix in place
    would be the complete version of this exit criterion.

Phase 10 (Field Analyzer) is accepted as planned scope; whether it's
required before `v1.0.x` is an explicit decision deferred until Phase 9
hardware evidence exists (ROADMAP.md).

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

- **Cell hardware verification** (Phase 11, above) — C key, mutual
  exclusion against Probe/Sweep (both directions), and the carousel card
  are now confirmed on real hardware (2026-09-01). Still open: confirm a
  real cell-band RSSI reading actually rises near a known tower, and
  confirm `cell.csv`/`session.csv`'s new columns write correctly to SD.
- Phase 9's endurance soak (scoped to 8h) ran 2026-09-02 with real,
  useful findings, but two of them are open, unresolved anomalies, not
  a clean pass: an unexplained recurring timing tail (~3.5% of laps),
  and WiFi silently going quiet ~30min after being switched on with no
  corresponding log or known code path. "Bounded memory" itself also
  hasn't been directly confirmed yet (`session.csv` not yet pulled from
  this run) — see the Phase 9 section above for the full breakdown. The
  other four exit criteria (timing/home-away duration, WiFi on/off,
  CAD-never-promotes-alone, low/mid/high bin accuracy) closed cleanly,
  2026-09-02.
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
