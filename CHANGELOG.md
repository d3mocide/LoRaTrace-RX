# Changelog

Full pre-2026-08-29 history (session-by-session development log, every
decision and debugging story) is archived unedited at
[docs/history/CHANGELOG.md](docs/history/CHANGELOG.md). This file starts
fresh from the documentation restructuring below and stays terse —
one or two lines per entry, newest first. For the current state of the
project (not a log of how it got there), see [docs/STATUS.md](docs/STATUS.md).

## 2026-09-05

- Ran Workstream 12's §6.2 controlled dwell matrix: 900 trials in 54.7 min,
  zero transport errors, zero drops, home restored on every trial; 14 of 15
  arms separated a controlled source from ambient 30/30 vs 0/30. The finding
  that matters is a limit — **detection tracks the source's airtime against
  Focus's sample spacing, not dwell length**. With a fixed 8 samples, spacing
  is dwell/7, so a 2000 ms dwell observes eight instants rather than 2000 ms;
  worst-case detection degraded monotonically as source airtime approached
  that spacing and one arm stopped separating. That is §3's "observation time
  is not coverage" rule with numbers behind it, and it means a later slice
  should scale samples with dwell. Bin-center offset turned out not to be the
  dominant term after all. No single fixed RSSI condition separates every arm,
  so §3.1 gets a candidate rather than a constant. Summary in
  `docs/hardware-results/2026-09-04-phase12-focus-matrix.md`.

- Two methodology bugs found by running the §6.2 matrix rather than by
  reading it. A single armed pulse at a fixed delay cannot cover every dwell
  arm — at 100 ms the pulse began after the window closed, and source-on was
  indistinguishable from source-off across 18 trials. Re-arming continuously
  fixed that but at SF12's ~275 ms airtime the sends overlapped and their tail
  bled into the next trial, making source-off read -26 dBm. The runner now
  paces the burst by the transmitter's own `TX_DONE` and waits for the tail to
  clear before a trial ends: 100 ms arms then separate cleanly (-25/-27 dBm
  against -97/-101 dBm). Both failure modes are recorded in the design doc so
  the next harness doesn't rediscover them.

- Added `scripts/phase12_watch_opportunity.py` for §6.3, the measurement that
  gates Focus's maximum radio-away budget: Watch-only versus Watch-with-Focus
  against one independently timed pulse train, reported as Wilson intervals
  plus away time, completed requests, and restored-Watch gap distribution. It
  refuses to run unless the device's home channel matches the reference train,
  since otherwise both arms hear nothing and their equality would read as a
  reassuring result, and it reports the away fraction alongside the loss
  because a small loss at a small duty cycle predicts nothing about a larger
  one.

- Built Workstream 12's §6.2 matrix tooling: `scripts/phase12_focus_matrix.py`
  runs paired source-on/source-off Focus trials across dwell arms and writes
  one durable JSONL row each, and `scripts/phase12_focus_matrix_report.py` does
  the offline analysis — per-arm distributions, the lowest separating 1 dB
  condition or an explicit "these overlap", and Wilson 95% intervals rather
  than bare proportions. Neither can emit a coverage label. Validated on
  hardware with a 4-trial smoke run at 912.750 MHz: source-on P90 -66/-68 dBm
  against -97/-98 dBm quiet, 573 ms radio-away, no drops.

- Found that none of §6.1's three fixture frequencies sits on a Focus bin
  center. Focus tunes at the home channel's bandwidth (125 kHz here), so the
  offset changes what a pass can observe: low and mid are +62.5 kHz off bins 13
  and 43, and the high position sits exactly on a bin boundary, 125 kHz from
  either neighbour — outside the passband. A null there would measure geometry,
  not sensitivity, so the matrix now also runs two exactly bin-centered
  controls from the same transmitter table, and every trial records its offset.

- Closed Workstream 12's last Engineering-gate item by measuring the Focus
  budgets off the built image instead of estimating them
  (`docs/research/phase12-survey-truth-design.md` §4.2): 12 B request, 40 B
  result, 148 B histogram, 188 B working state against a 256 B target, 159 B
  of Focus statics, a 1,072 B `radioTask` frame in a 6,144 B stack, and a
  189 B worst-case `focus.csv` row against a 256 B buffer that drops rather
  than truncates. Everything landed inside the bounds §4.1 had set, so
  nothing needed resizing. The row buffer now comes from one shared
  `FOCUS_CSV_ROW_MAX` and a test formats the saturated row, so the writer and
  its budget can't drift the way `STATUS`'s hand-sized buffer did.

- Phase 12: bounded a Focus request in wall-clock time, not just in samples.
  Each sample waits on the shared SPI bus, so a contended bus could stretch a
  pass past its dwell with the radio away from home and nothing to stop it —
  `focusRuntimeTimeout()` existed but no code path reached it. A request now
  carries a dwell-plus-slack deadline and terminates as `timeout`, restoring
  home like every other exit. Bench-only additions make the remaining Device
  gate reproducible: `BENCH_FOCUS`'s optional 4th field arms a one-shot
  sample-loop stall (production only times out under real contention), and
  `BENCH_ACTION` starts/cancels/reports Cell and Scope, which are menu-only in
  production, so Focus's mutual exclusion against them can be driven by a
  fixture. Verified on hardware the same day: a stalled request timed out,
  restored home, and wrote its durable row inside a 1,573 ms radio-away
  window, and Cell and Scope each arbitrated with Focus in both directions.

- Fixed a latent silent-drop in Serial Control's `STATUS`: its argument buffer
  was hand-sized to 240 bytes against a frame budget of ~230, and
  `serialControlFormatFrame()` drops an over-long frame rather than truncating
  — so a long session's wider counters would have lost the whole status frame,
  newest fields first, with no error. The argument budget is now derived from
  the frame size, the frame limit is 384, and a host test formats at
  saturation. The formatter also builds in place instead of staging through a
  second full-size buffer, which nets less stack on the 4KB UI task than
  before.

- Phase 12 engineering: wired one bounded, bench-only Focus Survey through
  Core 1, the Core-0 `focus.csv` writer, and restore-before-publish terminal
  state. Added framed `BENCH_FOCUS` and GPS-free compact
  `BENCH_FOCUS_RESULT` readback, durable-write health counters, and the
  explicit-transmit-gated `scripts/phase12_focus_bench.py` plus
  non-transmitting `scripts/phase12_focus_behavior_bench.py` fixture
  harnesses. Quiet/source-on, cancel, injected-failure, and Probe/Sweep
  arbitration hardware checks completed with successful home restore and SD
  rows; they do not set a coverage/activity threshold.

- `pages.yml`'s "Download stable release assets" step now retries (6x/20s)
  instead of failing hard: a maintainer publishing a draft release before
  release.yml's build job finishes uploading assets makes `release:
  released` fire before assets exist, which hit v1.0.7 twice
  (runs 33912509081/33912608422).

- Moved Tools and Analyze from main-carousel hub pages into real menu
  groups (`v1.0.7`), reversing part of the previous day's "ordinary
  carousel stop, no menu shortcut" call for those two — an operator report
  that having both a card-like home carousel and a separate menu read as
  "am I in the menu or not". Root menu is now Profile/Analyze/Tools/System;
  Trace moved from its own root row into Tools. Main carousel shrank from
  six stops to four (Radio/Channel/GPS/System); Probe/Sweep/Cell/Meter/
  Waterfall/Scope/Captures/Nodes are unchanged as real pages but reached
  only through the menu now.
- Two bench-pass follow-ups to the above, same day: restored the per-row
  live status (SCANNING/COMPLETE/dBm/etc.) Tools/Analyze rows lost when
  their hub pages folded into plain menu rows — `menuEntryValue()`
  (ui_pages.cpp) now covers every `OPEN_*` action, same state logic the
  deleted hub pages used. And `drawMenuList()` now scrolls any list longer
  than 4 rows (Analyze's 5 were overflowing under the footer hint text),
  keeping the selected row in view with a small `^`/`v` cue when more rows
  sit outside the window.
- Third same-day follow-up: on a Tools/Analyze sub-page, PREV/NEXT
  (left/right) now alias UP/DOWN and cycle the sibling pages in that group
  instead of leaving to the menu — "helps these tool carousels work like
  the main carousel." BACK is the sole "leave to the menu" key now.
- Fourth: BACK that closes the menu all the way out now returns to Radio
  if you'd left `page` on a Tools/Analyze sub-page — previously closing out
  after browsing System (say) re-showed whichever Probe/Sweep/Cell/Meter/
  ... page had been open before, not the main carousel.
- Fifth: Radio's own status page now banners a background repeat Sweep or
  repeat Cell scan, same shape as its existing Probe banner — previously
  neither showed at all on Radio. Sweep reads "REPEATING" (its capture
  window still listens between laps); Cell reads "WATCH PAUSED" (it has no
  such window and fully owns the radio while scanning, like Probe).
- Sixth: added Activity, a new main-carousel page at slot 2 (Radio,
  Activity, Channel, GPS, System) that mirrors whichever bounded action
  (Probe/Sweep/Cell/Scope) is currently running, full-panel, with an IDLE
  state when nothing is — a follow-on to the fifth item, giving that same
  status more room than Radio's own banner has. Read-only, same as the
  banner: no SELECT/REPEAT of its own, so it can't become a second way to
  start a scan.
- Seventh, bench feedback ("really bland and has a lot of dead space"):
  Activity now shows real live detail per tool (progress bar for Probe,
  the same frequency-position/occupancy/best-signal/lap numbers Sweep and
  Cell's own cards show, Scope's tuned frequency) instead of a bare state
  word, and its idle view lists all four tools' last result instead of a
  flat "nothing running" line — reading STANDBY instead of IDLE when Trace
  is paused with nothing else active.
- Eighth: dropped drawRadioPage()'s own STANDBY/Probe/repeat-scan banner
  now that Activity covers the same ground properly — its one case
  Activity's idle view didn't already have (STANDBY) is why that view now
  checks Trace-paused too, so removing the banner didn't quietly drop that
  signal everywhere.
- Ninth, more bench feedback: Activity's idle view drops its hero line
  (redundant — most tools read "IDLE" there most of the time anyway) and
  shows each tool's real last-result numbers instead of that perishable
  state word — hit count, peak count + best MHz, best MHz + dB, last
  Scope sample's dBm. Net effect: undoes the eighth item's STANDBY-vs-IDLE
  distinction — Trace-paused now shows nowhere except Menu > Tools > Trace,
  flagged as a real tradeoff rather than silently dropped.
- Tenth: that tradeoff turned out to matter — STANDBY is back on Radio,
  just the one line, not the rest of the old banner (Activity owns the
  Probe/Sweep/Cell/Scope detail now). True whenever watch isn't actively
  listening for any reason, manual pause or a bounded action running.

## 2026-09-04

- Archived the completed v1 roadmap at the immutable `v1.0.7` tag and
  streamlined `docs/ROADMAP.md` into the active V2 gate board. Updated
  `CLAUDE.md`, `AGENTS.md`, and documentation indexes so future work opens
  V2 gates/design first and searches v1 history only when needed.

- Opened V2 Workstream 12 (Survey truth) at Design entry: one selected-bin
  Focus Survey, fixed streaming statistics, durable `focus.csv` contract,
  controlled transmitter/RTL-SDR truthfulness matrix, and Portland metro,
  Oregon as the privacy-preserving field-validation area. No firmware scope
  has entered implementation; radio-away and coverage thresholds remain
  evidence-gated.

- Began Workstream 12 Engineering with host-tested, radio-free Focus request
  and `focus.csv` contracts: a single sourced bin, 1 dB bounded RSSI histogram
  (median/P90/peak), explicit completion states, and an intentionally blank
  `coverage` column. No menu, radio task, logger, or coverage conclusion is
  wired yet.

- Tightened the Focus bench-request contract before radio integration: exactly
  one pass, 2--2,000 ms dwell, and 2--64 samples. Added a host-tested state
  machine that records restoration before a terminal result and makes restore
  failure override partial/cancelled work.

- Wired the bench-only Focus vertical slice: a Core-1 one-bin RSSI request,
  mutually exclusive ownership/restoration, four-row Core-1-to-Core-0 queue,
  durable `focus.csv`, and framed `BENCH_FOCUS bin:dwell_ms:samples` control.
  The production image rejects the command; hardware evidence is still open.

- Ran a `/code-review` pass and then a whole-project audit
  (`docs/research/2026-09-04-project-audit.md`), and fixed what it found
  (`v1.0.6`). Radio-ownership bugs in v1.0.5's new capture window:
  `energyActive` didn't cover the window, so Probe/Cell/Scope's mutual
  exclusion silently queued instead of refusing and a repeat-stop press
  waited out the full budget; captures from a window no lap reported could
  be claimed by a later single-shot sweep, drawing a green Waterfall mark
  for packets it never received; the Pass-A margin is now snapshotted per
  sweep instead of read live per bin.
- Hardened two latent cross-core paths the audit found: `activeChannel` is a
  multi-word struct written on Core 1 and read from six Core-0 sites, now
  under a spinlock (Scope derives its tune frequency from it, so a torn read
  had teeth); and the per-sweep completion snapshot is now published behind
  an explicit release/acquire fence pair rather than relying on `volatile`
  on the counter to order a plain array — the same path that produced
  v0.10.1's all-quiet Waterfall rows.
- Fixed the Pages/tag CI failure: `release`-triggered runs carry the tag as
  their ref and the `github-pages` environment only allows the default
  branch, so the job died at 0 steps ("Tag vX.Y.Z is not allowed to deploy").
  Now re-dispatched onto `main`, keeping the environment gate. **Unverified
  — needs a real published release to confirm.**
- Closed a real release-integrity hazard: `release.yml`'s manual dispatch
  built the dispatch branch rather than the target tag, guarded only by a
  version-string check that `main` happened to satisfy — so a backfill
  against `v1.0.5` would have overwritten it with different binaries. The
  dispatch now checks out the tag itself.
- **Audit M6, partially confirmed and partially retracted.** Sweep's light
  retune reads the noise floor **2.40dB low** — reproduced identically in
  5/5 independent runs, so `energy.csv`'s absolute RSSI has been slightly
  low since v1.0.2. But the claim that the under-read *grows with signal
  strength* (which would mean lost detections, not just wrong logs) is
  **withdrawn**: three attempts gave three contradictory answers, and a
  settle=0 control re-running the exact "confirmed" configuration failed to
  reproduce it — FULL's own carrier reading moved 11dB between runs, as
  large as the claimed effect. Cause was reading a max statistic over a
  luck-dominated sample (a 2s pulse train vs a 10-55ms bin visit). Built
  `BENCH_SWEEP_SETTLE` (0-50ms) for the proposed fix but ship it
  **disabled**: ~425ms/sweep is too much to pay for an unproven benefit.
  The audit doc records what a trustworthy measurement would need.
- Added a bench-only `BENCH_SWEEP_RETUNE=FULL|LIGHT` opcode and
  `scripts/phase9_retune_floor_bench.py`, then used them to confirm audit M6:
  Sweep's shipped light retune reports the noise floor **2.84dB lower** than
  a full `begin()` per bin (-122.77 vs -119.93dBm, far outside each arm's
  0.05-0.54dB spread), so `energy.csv`'s absolute RSSI has been slightly low
  since v1.0.2. The switch is a runtime override so both arms run on one
  image in one session rather than across two builds. The bigger question —
  whether strong signals are suppressed *more* than the floor, which would
  cost real detections rather than just logging accuracy — needs the Heltec
  rig and is written up as Phase 2 in the audit doc.
- Tried porting Sweep's light per-bin retune to Cell (audit L2) and
  **reverted it**: 3.9x faster (5594ms → 1423ms) but it stopped seeing the
  band's strongest real carrier entirely — full `begin()` found 892.0MHz at
  −73dBm on 6/6 laps, the light retune never found it once and reported
  −86..−91dBm noise at a different frequency each lap. Cell reports absolute
  RSSI with no relative threshold to hide an under-read behind, so the
  speedup isn't worth it. A 5ms settling delay recovers stable −72dBm
  readings at 2.6x, which points at AGC settling time as the cause — but it
  picks a different strongest bin than the baseline, so equivalence is
  unproven. Kept the proven path; findings and an acceptance test are in the
  audit doc. This also raised a new open question (M6) about whether Sweep's
  own shipped light retune has the same under-read.
- Consolidated the four settings parsers behind a shared, pure
  `config_line.h` and moved each module's `apply...()` into its header, so
  they are finally host-testable (audit L1/M5) — 20 new cases, 207 → 227,
  and the `.cpp` files drop 401 → 293 lines. Doing so exposed a real bug the
  missing tests had hidden: `String::toInt()` returns 0 for unparseable
  input, and 0 is *valid* for two of these keys, so a corrupt
  `window_index`/`idle_timeout_index` line silently selected "OFF" instead
  of being ignored. Parsing is now strict. `BRIGHTNESS_MIN/MAX/STEP` also
  stopped being defined in two places.
- Extended the menu `itemCount` static_asserts to `ROOT_ITEMS`, which is
  where the v0.8.9 bug they were written for actually lived; verified by
  reintroducing that bug and confirming the build fails. Added the 18
  modules missing from `CLAUDE.md`'s layout.

## 2026-09-03

- Closed Phase 10 (Field Analyzer, `v0.10.0`→`v0.10.1`): all five exit
  criteria hardware-confirmed, including a 60-minute worst-case run (WiFi
  on, Sweep repeat, Waterfall open) with zero drops or watchdog resets.
  That run surfaced a real repeat-mode Waterfall bug — a cross-core race
  cleared the peak-bin mask before it could be read — fixed same day.
- Fixed a bench SD card causing a 100%-reproducible boot-loop, isolated by
  reproducing the identical crash on the last tagged release before
  concluding it wasn't a code regression.
- Shipped Waterfall's frequency axis and an Enter-to-toggle-repeat-Sweep
  control, then merged the axis into the plot box's own border to remove
  a redundant line (`v0.10.2`→`v0.10.3`). Gave Meter a real bar gauge, SNR
  line, and channel-param column — real `CaptureSummary` data it already
  had and never showed. All hardware-confirmed; every layout choice
  workshopped in `docs/research/analyzer-preview.html` first.
- Diagnosed a "Sweep sees nothing near real traffic" puzzle down to Sweep's
  own short dwell time, not the noise-floor margin — confirmed via the
  operator's own MeshCore repeater API and a direct RTL-SDR capture during
  a live Sweep. Added `RXP`/`RXC` (real RX/CRC counters) to Serial
  Control's `STATUS` line along the way.
- **Promoted to `v1.0.0`.** ROADMAP.md's own documented gate for this
  promotion was Phase 10 closing, and only that — done, same day. Phase 11
  (Cell) was never part of the gate; its two open items (a real cell-band
  RSSI rise near a tower, `cell.csv`/`session.csv` column verification)
  are known, tracked gaps post-`v1.0`, not blockers.
- Made Sweep's Pass-A peak margin operator-adjustable (`v1.0.1`):
  System > Tuning > Margin, 15.0-50.0dB in 5.0dB steps, persisted to
  `/loratrace/sweep_margin.txt`. Prompted by a field report of weak
  (-55 to -72dBm) deck-range readings, after the dwell-timing
  investigation above had already cleared the margin as the cause at a
  much stronger 6ft/59dB-clearance test — a weaker signal's clearance can
  still drop under the 35dB default even with clean SNR at the receiver.
  Region moved into a new nested "Tuning" group alongside it so System's
  own list stays at 4 rows.
- Cut Sweep's per-bin retune cost ~4.1x (`v1.0.2`): `performEnergySweep()`
  called a full `radio.begin()` (hardware reset, chip re-detect, TCXO
  restart, full config reload) for every bin, even though only frequency
  changes bin to bin — replaced with one real `begin()` per sweep plus a
  three-SPI-command `standby()`/`setFrequency()`/`startReceive()` retune
  for the rest. Hardware-confirmed at a fixed 35.0dB margin (zero Pass-B
  noise either side): 3463ms avg → 850ms avg across four 85-bin US-region
  sweeps each way. Also surfaced a bigger, separate cost: Pass-B's own
  bounded receive-on-hit window can now add up to ~20s to a sweep when the
  margin is sensitive enough to find several peaks — evaluated and left
  as-is this session (docs/STATUS.md).
- **Repeat-mode Sweep now captures real packets instead of going blind
  (`v1.0.3`).** After each lap it parks on the home channel with RX armed
  for 2s and services packets through the same path Trace uses. Measured
  against the operator's pyMC repeater as ground truth, back-to-back
  4-minute windows: **0/42 packets (0.0%) before, 22/27 (81.5%) after**,
  zero CRC errors, lap time unchanged. Also reverted `v1.0.2`'s
  samples-per-bin 4→34: real packet airtime (142-490ms) is orders of
  magnitude longer than any per-bin dwell, so capture is bounded by share
  of time parked on the channel, not by dwell width — which is why no
  amount of sample-count tuning could reach the operator's 50% target and
  timesharing cleared it on the first try. Pooled across both treatment
  runs: 30/44 = 68% (95% CI 54-82%).
- Added System > Tuning > Capture (Off/1s/2s/4s, `/loratrace/capture.txt`)
  so the survey-cadence-vs-capture trade is an operator choice rather than
  a silent default, per CLAUDE.md's house rule. Also added compile-time
  cross-checks that every nested menu group's hand-written `itemCount`
  matches its array length — the stale-count bug that silently hid the
  Region row in `v0.8.9` is now a build error, verified by deliberately
  breaking it.
- **Waterfall now shows packet captures, not just energy (`v1.0.5`).**
  Green = a packet was demodulated and CRC-checked on the home channel
  during that row's listen window; yellow stays energy-over-margin. Green
  routinely appears without yellow, because Pass A's per-bin glance is
  milliseconds against a 142-490ms packet — so the page no longer reads a
  flat "QUIET" while traffic is actively being captured. Stored as its own
  row channel rather than packed into the RSSI byte, and refused rather
  than clamped when the bin falls outside the swept range. Verified over
  45 consecutive rows on hardware (bin 34 = 910.525MHz, 15 rows carrying
  21 packets). Analyzer footprint 6728 → 6824 of 8192 bytes.

## 2026-09-01

- Added a Sweep region setting (`v0.8.9`): System > Region narrows
  Sweep's scanned band to 902-923MHz (US, the default) instead of the
  full 868-923MHz hardware range (Global), cited to 47 CFR § 15.247,
  roughly halving scan time for the common case. Persisted to
  `/loratrace/region.txt`. Cell and `channel_plans.h` are explicitly not
  region-aware yet — noted as separate follow-ups in `docs/ROADMAP.md`.
  Code + host-native tests only — not yet hardware-verified
  (`docs/STATUS.md`).
- Added FCC A/B block markers to the Cell frequency bar (`v0.8.8`): labels
  the FCC's own 869-880/890-891.5MHz (Block A) and 880-890/891.5-894MHz
  (Block B) split, cited to 47 CFR § 22.905. Regulatory block letter only
  — deliberately no carrier name, since actual current licensee varies by
  market and isn't a fixed national fact. Code + host-native tests only —
  not yet hardware-verified (`docs/STATUS.md`).
- Made R's repeat toggle page-gated (`v0.8.7`): it now only acts on the
  Sweep or Cell card (no-op elsewhere, including with the menu open),
  instead of firing globally like it did for Sweep alone. Cell gained the
  same repeat mode Sweep already had; Probe still has none by operator
  decision ("Repeat only on the Sweeps"). Hardware-verified same day —
  page-gating, Cell's lap counter, and mutual exclusion across a real
  repeat chain all confirmed (`docs/STATUS.md`).
- Added Cell (`v0.8.6`): a bounded RSSI-only presence sweep of
  869-894MHz (North American Cellular downlink), operator-requested after
  real wardriving runs picked up energy in that band near cell towers. Not
  a decode of any kind (the SX1262 cannot demodulate GSM/CDMA/LTE) and not
  a fifth mission profile — a third bounded radio-owned action alongside
  Probe/Sweep (own global hotkey C and carousel card, same as those two),
  isolated from `ENERGY_SWEEP`'s calibrated Pass A/B engine.
  See `docs/ROADMAP.md`'s Phase 11 entry and `docs/DESIGN.md` §5a. Code +
  host-native tests only — not yet hardware-verified (`docs/STATUS.md`).

## 2026-08-29

- Restructured docs: moved `DESIGN.md`, `ROADMAP.md`, `LOG_GUIDE.md`,
  `HARDWARE_TESTING.md`, `BRAND.md`, `research/`, and `hardware-results/`
  into `docs/`; archived `PROGRESS.md` and the prior `CHANGELOG.md`
  verbatim to `docs/history/`; added `docs/STATUS.md` as the single
  current-status doc (replacing duplicated status prose that had drifted
  across `CLAUDE.md`, `PROGRESS.md`, and `README.md`) and `docs/README.md`
  as a docs index.
