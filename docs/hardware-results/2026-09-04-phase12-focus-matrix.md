# Phase 12 §6.2 controlled dwell matrix — 900 trials

**Build:** `V=1.0.7;R=3e31daa-dirty;BENCH=1` (bench image; production rejects
every command used here).
**Date:** 2026-09-04. **Location:** bench, indoors. No coordinates, route, or
observed third-party identity appears here or in the tracked artifacts.
**Fixture:** repo-owned Heltec V4 R8 controlled transmitter, output capped at
-9 dBm, quieted on every exit path.
**Raw evidence:** `private/phase12-matrix-20260904T223047Z.{jsonl,log}`
(git-ignored). Re-derive this summary with
`scripts/phase12_focus_matrix_report.py`.

900 trials — 5 positions x 3 dwell arms x 30 source-on and 30 source-off,
alternating — completed in 54.7 min with **zero transport errors, zero queue
drops, zero dropped durable rows, and successful home restore on every trial**.
Each trial was one independently logged bounded Focus request.

## Result by arm

Ordered by the transmitter's measured airtime, which is what the outcome
actually tracks. `margin` is the gap between the worst source-on reading and
the strongest source-off reading; a negative margin means the sets overlap.

| Position | tx airtime | offset from bin center | dwell | source-on min | source-off max | margin |
|---|---|---|---|---|---|---|
| low (905.3125) | 1751 ms | +62.5 kHz | 100 ms | -29.0 | -94.0 | 65.0 dB |
| low | 1751 ms | +62.5 kHz | 500 ms | -29.0 | -95.0 | 66.0 dB |
| low | 1751 ms | +62.5 kHz | 2000 ms | -29.0 | -75.0 | 46.0 dB |
| mid (912.8125) | 1026 ms | +62.5 kHz | 100 ms | -29.0 | -97.0 | 68.0 dB |
| mid | 1026 ms | +62.5 kHz | 500 ms | -29.0 | -98.0 | 69.0 dB |
| mid | 1026 ms | +62.5 kHz | 2000 ms | -32.0 | -92.0 | 60.0 dB |
| low-aligned (908.750) | 286 ms | 0 kHz | 100 ms | -55.0 | -97.0 | 42.0 dB |
| low-aligned | 286 ms | 0 kHz | 500 ms | -61.0 | -97.0 | 36.0 dB |
| low-aligned | 286 ms | 0 kHz | 2000 ms | -71.0 | -97.0 | 26.0 dB |
| mid-aligned (918.500) | 148 ms | 0 kHz | 100 ms | -32.0 | -97.0 | 65.0 dB |
| mid-aligned | 148 ms | 0 kHz | 500 ms | -87.0 | -92.0 | 5.0 dB |
| mid-aligned | 148 ms | 0 kHz | 2000 ms | -33.0 | -97.0 | 64.0 dB |
| high (920.625) | 94 ms | +125 kHz | 100 ms | -45.0 | -97.0 | 52.0 dB |
| high | 94 ms | +125 kHz | 500 ms | -47.0 | -96.0 | 49.0 dB |
| **high** | 94 ms | +125 kHz | **2000 ms** | **-99.0** | **-96.0** | **-3.0 dB** |

In the 14 arms that separated, every one separated completely: 30/30 source-on
detected and 0/30 source-off, Wilson 95% intervals [0.886, 1.000] and
[0.000, 0.114].

## What the matrix established

**1. Detection tracks airtime against sample spacing, not dwell length.**
Focus takes a fixed 8 instantaneous RSSI samples spread across the dwell, so
sample spacing is `dwell / (samples - 1)`: 14 ms, 71 ms, and 286 ms for the
three arms. Detection holds while the source's airtime is comfortably longer
than that spacing and degrades as it approaches it:

| tx airtime | vs 286 ms spacing (2000 ms arm) | worst-case source-on across dwells |
|---|---|---|
| 1751 ms, 1026 ms | much longer | flat: -29, -29, -29 / -29, -29, -32 |
| 286 ms | comparable | degrades: -55, -61, -71 |
| 148 ms | shorter | erratic: -32, -87, -33 |
| 94 ms | much shorter | fails at 2000 ms: -45, -47, -99 |

This is the practical form of the §3 contract's warning that observation time
must not be read as time covering a frequency. **A 2000 ms dwell does not
observe 2000 ms — it observes eight instants.** Holding the sample count fixed
while lengthening the dwell makes the pass strictly worse at catching bursts,
which is the opposite of what "longer observation" suggests to a reader. Any
operator-facing presentation of dwell or observation time has to account for
this, and a later slice should scale the sample budget with the dwell rather
than leaving it fixed at 8.

**2. Bin-center offset had no detectable effect at these signal levels.** This
was the concern that motivated adding the two exactly-centered control
positions: the `high` position sits 125 kHz from its bin center, outside the
125 kHz home-channel passband half-width. It still separated by 52 dB and
49 dB at the two shorter dwells, while the perfectly centered `low-aligned`
position separated by only 42/36/26 dB. Offset is therefore not what
distinguishes these arms — airtime is. This does not prove offset is
irrelevant at realistic signal levels; it proves it is not the dominant term
at a source this strong.

**3. Radio-away time is dwell plus a small fixed overhead.** Measured 173 ms,
573 ms, and 2073 ms for the 100/500/2000 ms arms — a consistent ~73 ms of
retune-and-restore overhead, with a worst case of 2139 ms across all 900
trials. No trial lost radio ownership or failed to restore home listening.

## What it did not establish

**No single fixed RSSI condition separates every arm.** Pooled across all 900
trials the sets overlap (source-on min -99.0 dBm against source-off max
-75.0 dBm), and they still overlap with the one failing arm excluded
(-87.0 against -75.0). The nearest thing to a clean global condition is
`p90 >= -90 dBm`, which across the 14 separating arms misses no source-on
trial and takes 1 ambient false hit in 420. That is a **candidate** for §3.1,
not an accepted constant, and it carries three limits:

- The bench source reads around -26 dBm against a -100 dBm ambient floor —
  far stronger than realistic traffic. This separates a strong known source
  from ambient; it is **not** a sensitivity limit and must not be shown as one.
- Source-off trials are ambient observation, not a calibrated false-hit rate:
  the control path is not known quiet (§6.1). The single -75 dBm ambient
  reading is a real received event, not necessarily noise.
- A condition chosen on `p90` of 8 samples inherits finding 1: it describes
  eight instants, so its meaning changes with the dwell it ran at.

**The coverage thresholds remain unselected.** `FOCUS_MIN_VALID_PASSES`,
`FOCUS_MIN_OBSERVATION_MS`, and the `repeated` pair are about pass counts and
accumulated time across repeated requests; a single-pass matrix cannot supply
them. `insufficient`/`sampled`/`repeated` stay undisplayable, and `coverage`
stays blank in `focus.csv`.

**The §6.3 Watch-opportunity comparison has not run**, so the maximum
radio-away budget is still unapproved.

## Method notes worth not relearning

Two harness defects were found by running this, each of which produced
confident, wrong-looking data before it was caught:

1. **One armed pulse at a fixed delay cannot cover every dwell arm.** At
   100 ms the pulse began after the window closed, and source-on was
   indistinguishable from source-off across 18 trials — readable as "a 100 ms
   dwell detects nothing."
2. **Re-arming faster than the airtime overlaps transmissions**, and the tail
   bleeds into the next trial: source-*off* trials read -26 dBm — readable as
   "the receiver hears a transmitter that is switched off."

The runner now paces its burst by the transmitter's own `TX_DONE` and waits
for the tail to clear before a trial ends. A source-on trial consequently means
*the transmitter was radiating during the window*, which is the condition the
qualifying-RSSI question needs — and explicitly **not** a catch-probability
estimate for intermittent traffic.
