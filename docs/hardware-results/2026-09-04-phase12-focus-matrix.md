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


---

# Follow-up: sample-spacing sweep (360 trials)

**Raw evidence:** `private/phase12-spacing-20260905T010723Z.{jsonl,log}`.
360 trials, 31.5 min, zero transport errors, zero drops, home restored on
every trial. Same fixture and build family; home channel pinned to
918.5 MHz / SF8 / BW125 by bench build flags rather than by the SD card.

Finding 1 above said detection tracks the source's airtime against sample
spacing. This sweep tests that directly: dwell held at 2,000 ms while the
sample count varies, so spacing is the only thing changing.

| source airtime | 286 ms spacing (8) | 100 ms (21) | 50 ms (41) | 20 ms (101) |
|---|---|---|---|---|
| 94 ms (`high`) | 8/15, -3.0 dB | 2/15, -1.0 dB | **15/15, +2.0 dB** | **15/15, +24.0 dB** |
| 148 ms (`mid-aligned`) | 15/15, +53 dB | 15/15, +52 dB | 15/15, +54 dB | 15/15, +54 dB |
| 286 ms (`low-aligned`) | 15/15, +26 dB | 15/15, +25 dB | 15/15, +35 dB | 15/15, +36 dB |

`detects` counts source-on trials reading above that arm's strongest ambient
reading; `margin` is worst source-on minus strongest source-off.

**Detection collapses once spacing approaches the source's airtime.** The
94 ms source is missed at 286 ms and 100 ms spacing and caught in every trial
at 50 ms and 20 ms. Its worst-case reading also improves sharply between those
two (-97 dBm at 50 ms, -66 dBm at 20 ms), so 50 ms is the edge of working
rather than a safe choice. The two longer-airtime sources separate at every
spacing tested, and their worst case still improves as spacing tightens
(-71 to -64 dBm for the 286 ms source).

**Finer sampling costs nothing measurable.** Radio-away time was 2,073-2,075 ms
across all four arms, whether the pass took 8 samples or 101:

| samples | spacing | radio-away (min/max) | observation_ms |
|---|---|---|---|
| 8 | 286 ms | 2,074 / 2,075 ms | 2,000 ms |
| 21 | 100 ms | 2,074 / 2,075 ms | 2,000 ms |
| 41 | 50 ms | 2,073 / 2,074 ms | 2,000 ms |
| 101 | 20 ms | 2,074 / 2,074 ms | 2,000 ms |

The cost of a longer dwell is the dwell. The sample count inside it is free,
and the histogram is a fixed 141 bytes regardless (§4.2), so there is no
memory argument for coarse sampling either.

## Selected policy

`FOCUS_SAMPLE_SPACING_MS = 20`, with `focusSamplesForDwell()` deriving the
count as `ceil(dwell / spacing) + 1`. Rounding up matters: truncating would
let a 30 ms dwell take two samples 30 ms apart and violate the constant it is
derived from. This keeps roughly a 2x margin against the ~40-50 ms airtime of
the fastest realistic mesh traffic, which is a judgement about what is worth
observing rather than a measured limit -- the measurement says only that
spacing at or above the airtime fails and half of it works.

The `p90 >= -90 dBm` qualifying-condition candidate from the main matrix was
measured under the old 8-sample policy and a source ~70 dB above ambient. It
should be re-selected against this sampling and a weaker source before any
operator-facing use.


---

# Follow-up 2: qualifying condition at a realistic signal level (120 trials)

**Raw evidence:** `private/phase12-step2-20260905T035952Z.{jsonl,log}`.
120 trials (2 positions x 30 source-on + 30 source-off), 9.4 min, zero
transport errors, zero drops, home restored on every trial. 2,000 ms dwell at
the measured 20 ms sampling policy (101 samples per pass).

The earlier matrix chose `p90 >= -90 dBm` as a candidate qualifying condition
while the source sat ~70 dB above ambient. This run repeats the question at a
level a field deployment might actually see: transmitter on an outdoor porch,
driven over the WiFi control bridge, receiver with its antenna fitted so the
ambient floor is real.

**Configuration:** source-on peak reads about -85 dBm against a -99/-100 dBm
ambient floor, versus -24 dBm against -98 dBm on the bench. Receiver antenna
fitted; transmitter at roughly one building's distance with a small stubby
antenna.

## The candidate does not survive

| metric | low-aligned (908.75) | mid-aligned (918.5) |
|---|---|---|
| `p90` (the previous candidate) | overlap | overlap |
| `peak` | separates by **2.0 dB** | overlap |
| `peak - median` | separates by 2.0 dB | overlap (-2.0 dB) |
| `peak - p90` | 1.0 dB, 3 false positives in 30 | overlap (-3.0 dB) |
| `p90 - median` | overlap | overlap |

One of ten combinations separates, by 2 dB. That is inside ordinary RSSI
variance and would not survive a temperature change, a different antenna, or a
different room. **`p90 >= -90 dBm` is rejected**, and so is every relative
(floor-referenced) variant tested here. Both families fail for the same
reason, so this is not an artifact of picking the wrong statistic.

Two observations explain it:

- **`p90` collapses at low SNR even when the source is present most of the
  time.** At `low-aligned` the transmitter radiated 57-71% of the window, yet
  `p90` still read -94 to -97 dBm against a -100 floor. If the received signal
  were steady, `p90` would equal `peak`. It does not: at these levels the
  instantaneous RSSI during a weak transmission mostly fails to stand above
  noise, and only its best moments do. That is why `peak` outperforms every
  other summary here, and why none of them is reliable.
- **Real traffic is indistinguishable from the controlled source.** At
  `mid-aligned` (918.5 MHz, a live MeshOregon channel) source-off trials
  reached -94 dBm. That is not measurement error; it is a real transmission
  arriving during a control trial. An RSSI condition cannot tell a controlled
  fixture from a neighbour's node, and should not be expected to.

## What this means

An RSSI *summary statistic* cannot carry an activity claim at realistic signal
levels. Coverage reporting is unaffected: valid passes, observation time, and
the median/P90/peak summary itself remain honest and useful. What the evidence
refuses is the inference from "RSSI was elevated" to "something transmitted" —
precisely the inference §3's contract was written to prevent. The design was
right; the constant was wrong.

Two untested routes remain, in order of cost:

1. **A count, not a statistic.** `peak` is one sample and therefore noise-prone;
   counting how many of a pass's 101 samples exceed an adaptive floor
   integrates instead. `FocusObservation` already reserves `qualifying_count`
   for this and never populates it. Needs firmware, and can reuse this fixture.
2. **CAD or packet evidence**, which §3 already contemplates as the alternative
   basis for observed activity. A different primitive, and a larger change.

Until one of those is measured, `coverage` stays blank, the activity count
stays unpopulated, and Focus reports what it observed rather than what it
concludes.
