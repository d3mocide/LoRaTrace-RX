# Changelog

Full pre-2026-08-29 history (session-by-session development log, every
decision and debugging story) is archived unedited at
[docs/history/CHANGELOG.md](docs/history/CHANGELOG.md). This file starts
fresh from the documentation restructuring below and stays terse —
one or two lines per entry, newest first. For the current state of the
project (not a log of how it got there), see [docs/STATUS.md](docs/STATUS.md).

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
