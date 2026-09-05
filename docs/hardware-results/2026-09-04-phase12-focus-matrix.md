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


---

# Follow-up 3: Watch-opportunity comparison (§6.3)

**Raw evidence:** `private/phase12-watch-20260905T042817Z.{jsonl,log}`.
Two 240 s arms against one independently timed reference train (`MESH_OREGON`,
918.5 MHz SF8/BW125, one pulse every 2 s), transmitter outdoors on the WiFi
control bridge, receiver on its resolved home channel. Zero CRC errors in
either arm. Focus requests were 2,000 ms dwell at the measured 20 ms sampling
policy (101 samples).

| arm | reference | received | fraction | 95% CI |
|---|---|---|---|---|
| Watch only | 120 | 106 | 0.883 | [0.814, 0.929] |
| Watch + Focus | 121 | 56 | **0.463** | [0.377, 0.551] |

Focus completed 56 requests, was refused none, and held the radio for
116,142 ms — **48.1% of its arm**. Time between restored Watch windows:
median 4.04 s, maximum 7.45 s. The intervals do not overlap, so the
difference is not a small-sample artifact.

## The cost is proportional, with nothing hidden

If Focus simply stops the receiver hearing anything while away, and costs
nothing else, the predicted reception is the baseline scaled by the time
remaining at home: `0.883 x (1 - 0.481) = 0.459`. Measured: **0.463**. The
difference is well inside the interval.

That is the useful part of this result. It says the loss is entirely
accounted for by away time — there is no additional penalty from retuning,
from recovery, or from any lingering effect once home listening is restored.
**The radio-away duration Focus already records is an honest proxy for what a
request costs Watch**, which is what makes an operator-facing away-time
display meaningful rather than decorative.

The other half is that there is no mitigation either. Half the listening time
away is half the packets. A displayed cost is still a cost.

## Limits

- The baseline is 0.883, not 1.0: about 12% of reference pulses were missed
  with no Focus running. These are relative figures against a real link, not
  absolute capture rates, and the comparison is valid only because both arms
  saw the same link and the same train.
- One duty cycle (48%) at one dwell (2,000 ms) at one pulse interval (2 s).
  Proportionality held here; it is not established across other duty cycles,
  and a much shorter dwell has proportionally more retune overhead per unit
  of observation.
- The reference train is a controlled fixture, not real mesh traffic. Real
  traffic is bursty and correlated in ways a fixed 2 s interval is not.

## What it does not decide

A maximum radio-away budget is an operator product decision, and this
measurement deliberately does not make it. What it supplies is the exchange
rate: **at this dwell, Watch packet opportunity falls in direct proportion to
the fraction of time Focus holds the radio.** Choosing what fraction is
acceptable — and whether Focus should bound it automatically rather than
leaving it to whoever is pressing the button — remains open.


---

# Follow-up 4: a count survives where every summary statistic failed

**Raw evidence:** `private/phase12-counts-20260905T050037Z.{jsonl,log}`.
120 trials (2 positions x 30 source-on + 30 source-off), 11.4 min, zero
transport errors, zero drops, home restored on every trial. Same porch
configuration and 2,000 ms / 101-sample passes as Follow-up 2, which rejected
every RSSI summary statistic at this signal level.

Follow-up 2 ended by naming one untested idea: `peak` is a single sample and
therefore noise-prone, so **count** the samples above an adaptive floor rather
than taking an extreme of them. The bench image now reports a ladder of counts
at median + 2/4/6/8/10/15/20 dB, so one run evaluates any such rule offline.

## Result

Pooled across both positions, best operating point **`C6 >= 2`** — at least two
of a pass's samples at or above that pass's own median plus 6 dB:

| | count | 95% CI |
|---|---|---|
| detected, source on | **57/60 (95.0%)** | [0.863, 0.983] |
| detected, source off | **1/60 (1.7%)** | [0.003, 0.089] |

Per position:

| position | detect | false |
|---|---|---|
| low-aligned (908.75) | 30/30 | 1/30 |
| mid-aligned (918.5) | 27/30 | 0/30 |

`C4 >= 5` performs identically (57/60 and 1/60); the ladder has a broad
plateau rather than one lucky point, which is what a real effect looks like.

For contrast, at this same signal level Follow-up 2 found `p90` and `peak`,
absolute and floor-relative, could not separate the two populations at all.

## The one false positive is not a false positive

The single source-off trial flagged by `C6 >= 2` read `peak = -63 dBm` against
a -101 dBm median — 38 dB above its own floor, with six elevated samples.
Nothing 38 dB over noise is noise. That is a real transmission that was not
ours, correctly identified by the detector and mislabelled by the experiment,
which cannot distinguish "our fixture is quiet" from "the band is quiet". The
true false-alarm rate against genuine silence may therefore be 0/60; this
evidence cannot separate the two, and the reported 1.7% should be read as an
upper bound.

The three misses are all mid-aligned at `peak = -95/-96 dBm`, within a couple
of dB of the floor. A detector that misses signals at the noise floor is
behaving correctly; one that claimed them would be the problem.

## What still has to be settled before this is a constant

**The threshold is a count out of 101 samples — about 2% — and the sample
count now scales with dwell.** Under the measured 20 ms sampling policy a
100 ms pass takes 6 samples, where "2 samples" is 33% rather than 2%. A fixed
count therefore means something completely different at another dwell. The
rule must be expressed as a fraction of accepted samples, or validated
separately per dwell, before it can be written into firmware. That is the next
measurement, not a detail to decide at a keyboard.

Also unestablished: one distance, one indoor/outdoor geometry, two
frequencies, one 2,000 ms dwell. And the margin is relative to the pass's own
median, which assumes the median represents the floor — true when the source
is present for a minority of samples, and less true as occupancy rises toward
50%.

Until those close, `qualifying_count` stays unpopulated and `coverage` stays
blank. What this run establishes is narrower and still worth having: **an
adaptive count is a viable basis for an activity claim where every summary
statistic measured here was not.**


---

# Follow-up 5: what the count rule actually detects

**Raw evidence:** `private/phase12-dwell-20260905T053804Z.jsonl` (240 trials)
and `private/phase12-occ-20260905T053804Z.jsonl` (240 trials). Zero arm
failures. Same porch configuration.

Follow-up 4 left two questions: is the rule a fixed count or a fraction of
samples, and does "median as the floor" survive rising occupancy. Both were
measured rather than argued.

## 1. A fraction, with a hard minimum sample count

Sample count now scales with dwell (20 ms spacing), so the same rule was
re-derived at each dwell:

| dwell | samples/pass | best rule | as a fraction | detect | false |
|---|---|---|---|---|---|
| 100 ms | 6 | `C4 >= 1` | 16.7% | **23/60 (38%)** | 1/60 |
| 500 ms | 26 | `C6 >= 1` | 3.8% | **60/60 (100%)** | 2/60 |
| 2,000 ms | 101 | `C4 >= 5` | 5.0% | 57/60 (95%) | 1/60 |

A fraction of roughly **4-5% of accepted samples** transfers between the
500 ms and 2,000 ms passes. It does not rescue the 100 ms pass, which fails at
every threshold: with six samples there is no rule that both catches the
source and rejects ambient. That is a floor on the instrument, not a tuning
problem — **a short pass can report coverage honestly, but must not report
activity at all.** 500 ms / 26 samples was the strongest arm measured.

## 2. Median-as-floor held, but was not tested where it should break

Across source duty from 28.6% to 57.2%, the pass median read -101.0 dBm for
source-on and source-off alike. It never tracked the signal, so the adaptive
floor is sound over the range tested.

The reason is worth stating, because it bounds the result: at these levels
most samples read like noise even while the transmitter is radiating, which is
the same effect that made `p90` useless in Follow-up 2. A **strong** source at
the same occupancy would pull the median up and break the assumption. This
sweep did not reach that regime and does not license a claim about it.

## 3. Detection is a function of occupancy, and that is the limitation

Same dwell, same rule, only the source's duty within the window varied:

| source duty | detect (`C6 >= 5`, n=30) | 95% CI |
|---|---|---|
| 57.2% | 27/30 (90%) | [0.74, 0.97] |
| 42.9% | 28/30 (93%) | [0.79, 0.98] |
| 28.6% | 11/30 (37%) | [0.22, 0.54] |
| 28.6% | 13/30 (43%) | [0.27, 0.61] |

Detection collapses below roughly 40% occupancy. **This rule detects a
persistently occupied channel; it does not detect individual packets.** A
single SF8 packet inside a 2,000 ms pass is on the order of 3-7% occupancy —
far below where this measurement shows the rule already failing more than half
the time.

That is not a defect to tune away. It is the honest scope of an RSSI-sampling
instrument: it can say "this frequency was busy while I listened", and it
cannot say "a transmission occurred". The latter needs a primitive that
detects a signal rather than sampling energy — CAD, or actual packet
reception, which §3 has contemplated as the alternative basis from the start.

## Consequences for the phase

- Any activity condition is a **fraction** of accepted samples (~4-5%), never
  a fixed count, and applies only above a minimum sample count. 6 samples is
  below it; 26 is comfortably above.
- An activity claim, if one is ever surfaced, must be worded as channel
  occupancy during the pass, not as evidence that something transmitted.
- Detecting individual packets needs CAD or packet reception. That is now the
  better-supported route for §3's "observed activity", and this sweep is the
  reason to prefer it rather than a hunch.
- `qualifying_count` remains unpopulated. What to populate it with is now a
  design decision with evidence behind it rather than an open guess.


---

# Follow-up 6: a second baseline, and a correction

**Raw evidence:** `private/phase12-b2-{occ,counts,dwell}-20260905T134743Z.jsonl`
(600 trials, no arm failures). Same porch position and geometry as Follow-up 5;
the only change is the link.

Every conclusion in Follow-ups 4 and 5 came from one link at roughly 12 dB SNR.
This repeats the SNR-sensitive tests at roughly 25 dB, to separate properties
of Focus from properties of that link.

**Link change:** the transmitter's stubby was replaced with a matched whip and
both antennas were stood vertical. Vertical alignment mattered more than the
antenna: at 918.5 MHz the horizontal pair was losing about 13 dB, consistent
with each whip sitting in the other's pattern null. Source peak moved from
-85/-86 dBm to -77/-66 dBm at 908.75 and from -87/-89 to -69/-75 at 918.5,
against an unchanged -98/-101 dBm floor.

## The occupancy cliff was an SNR limit — Follow-up 5's conclusion is withdrawn

| source duty | detect at ~12 dB | detect at ~25 dB |
|---|---|---|
| 57.2% | 27/30 (90%) | 30/30 (100%) |
| 42.9% | 28/30 (93%) | 29/30 (97%) |
| **28.6%** | **11/30 (37%)** | **26/30 (87%)** |
| 21.5% | not tested | 22/30 (73%) |

Follow-up 5 concluded that "the rule detects a persistently occupied channel,
not individual packets", and reasoned from there that CAD or packet reception
was the better-supported basis for an activity claim. **That conclusion does
not survive a second link.** At 28.6% occupancy, 13 dB of link improvement
took detection from 37% to 87%. The cliff moved, so it was not the statistical
limit it was presented as; it was where *that* link stopped delivering samples
above the margin.

What can be said now is narrower and conditional: **detection depends on
occupancy and SNR together**, and the occupancy at which it fails is a
property of the link, not of the instrument. Single-packet detection is
untested and is no longer ruled out — a 150 ms packet at 20 ms sampling yields
roughly seven elevated samples, comfortably above the four or five the best
rules require, provided the samples clear the margin. The lowest occupancy
measured is 21.5%, so anything below that remains unmeasured in both
directions.

## What did survive

- **The 100 ms floor is real.** Six samples per pass detected 23/60 at 12 dB
  and 28/60 at 25 dB. Thirteen decibels bought almost nothing, so this is a
  sampling limit rather than a link limit: a short pass may report coverage
  and must not report activity.
- **The rule is a fraction.** Best thresholds landed at 3.8-5.0% of samples at
  12 dB and 4.0-7.7% at 25 dB. Roughly stable, and clearly not a fixed count.
- **500 ms / 26 samples remains the strongest arm** at both SNRs (60/60 each),
  with zero false positives at 25 dB.

## Method note

This is why a single link is not a baseline. Three conclusions were drawn from
the first one; two held and one inverted. The one that inverted was the most
consequential and the most confidently worded, and it would have sent the next
slice toward CAD on the strength of an artifact. Where a conclusion depends on
signal level, it now needs at least two levels before it goes in this file.

## Next measurement

Drive occupancy below 21.5% at the stronger link — a single short-airtime
pulse per pass is about 5% — and find where detection actually fails when the
link is not the binding constraint. That, not a CAD rewrite, is the open
question for §3's activity basis.


---

# Follow-up 7: a single packet is detectable

**Raw evidence:** `private/phase12-singlepulse-20260905T173222Z.jsonl`
(120 trials, no arm failures). Same ~25 dB link as Follow-up 6. One armed
transmission placed inside each 2,000 ms pass instead of a burst — a burst
answers a different question however its duty is tuned.

| position | packet | occupancy | detect (`C6 >= 4`) | false |
|---|---|---|---|---|
| mid-aligned | 148 ms | 7.4% | **29/30 (97%)**, [0.83, 0.99] | **0/30** |
| low-aligned | 286 ms | 14.3% | 20/30 (67%), [0.49, 0.81] | 2/30 |

Source-on peak at mid-aligned: median -74 dBm against a -97 dBm floor.

**This settles the question Follow-up 5 got wrong.** That entry concluded the
rule "detects a persistently occupied channel, not individual packets".
Follow-up 6 withdrew it on the grounds that the occupancy cliff was a property
of a weak link; this measures the positive case directly. At an adequate link,
a single packet occupying 7% of the pass is detected essentially every time,
with no false positives in thirty controls.

The arithmetic behind it is unremarkable in hindsight: a 148 ms packet sampled
every 20 ms yields roughly seven elevated samples, comfortably above the four
the rule requires. Nothing about the instrument prevented this. The first
link did.

## low-aligned was contaminated, and that is informative

The weaker result at low-aligned comes with a source-off control that peaked
at **-57 dBm** — 44 dB above its own floor. That is real traffic on 908.75
during the run, not noise, and it cuts both ways: it produces the two false
positives, and by lifting the median in affected passes it suppresses counts
and produces misses. An adaptive floor is only as good as the assumption that
the floor is quiet.

Two consequences worth carrying forward. A frequency with real activity is
harder to measure on, not easier, so controlled work should prefer quiet
positions. And in the field the same effect applies in reverse: Focus will be
least reliable exactly where a band is busiest.

## Limits

- One link (~25 dB SNR) and one dwell. At the ~12 dB link of Follow-ups 4-5
  this would be substantially worse; the rate is not a property of Focus alone.
- The runner verifies that the pulse fired, not precisely *when* it fired
  relative to the window. An attempt to classify placement from log timestamps
  was invalid and discarded: `TX_STARTED` reaches the host only when the
  harness next polls the socket, so its timestamp records host polling rather
  than RF timing. 29/30 detection implies placement was mostly correct, but it
  is inferred rather than measured. Timing the pulse against the window needs
  a device-side timestamp, not a host-side one.
- 30 trials per cell. The intervals are wide enough to matter: [0.83, 0.99]
  is consistent with a true rate anywhere from about six-in-seven to
  essentially always.

## Where this leaves the activity claim

An RSSI-sampling pass **can** support a packet-level activity signal at an
adequate link, using a count above an adaptive floor. What it cannot do is
support it *unconditionally* — the same rule at a weaker link failed on
sources occupying four times as much of the window. Any operator-facing
activity indication has to be honest that it is a function of link quality,
which the device cannot know. That is a wording and product problem now,
rather than an open measurement.
