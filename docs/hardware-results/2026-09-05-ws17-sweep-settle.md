# Workstream 17 — Pass A reads a floor ~14 dB low as shipped

**Build:** bench image, `V=1.0.7;BENCH=1`. **Date:** 2026-09-05.
**Link:** Workstream 12's porch fixture at its stronger setting, matched whips
vertical, transmitter on `MESH_OREGON` (918.5 MHz -> Sweep bin 66 of 85).
**Raw:** `private/ws17-retune-probe.log`, `private/ws17-check*.jsonl`.

Entry measurement for the candidate workstream. `src/version.h` records an
11 dB under-read on Cell caused by sampling RSSI before the AGC settles after
a light retune, and states plainly that "Whether Sweep has the same under-read
is unknown". This measures it.

Each row: two silent laps and three laps with the transmitter radiating,
median of `BENCH_SWEEP_FLOOR` for bin 66.

| Pass A configuration | silent floor | source on | delta |
|---|---|---|---|
| **LIGHT + 0 ms settle (shipped)** | **-115.0 dBm** | -114.8 | +0.2 dB |
| LIGHT + 5 ms settle | -101.5 dBm | -102.5 | -1.0 dB |
| LIGHT + 20 ms settle | -101.7 dBm | -98.2 | +3.5 dB |
| FULL `begin()` per bin | -112.3 dBm | -112.5 | -0.2 dB |

## Established

**Pass A as shipped reads the noise floor about 14 dB below a settled
receiver.** Adding a 5 ms settle moves the silent floor from -115.0 to
-101.5 dBm, and 20 ms holds it at -101.7. Focus independently measures the
ambient floor at this frequency as -99 to -101 dBm across hundreds of passes,
which is where the settled readings land and is 14 dB above the shipped one.

That answers `version.h`'s open question: Sweep has the same defect Cell had,
and larger. It matters because Pass A's peak decision compares a bin against a
noise floor plus a margin, and both terms are being computed from under-read
samples. `ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10` was calibrated against
those same under-read values, so the margin is not obviously wrong — but it is
calibrated on a scale that does not correspond to real dBm, which makes any
absolute reasoning about Sweep sensitivity unsound until this is settled.

## Not established, and not to be inferred from this

**Whether Pass A can flag a bin carrying traffic remains open.** The
source-present column is not a clean measurement and should not be read as
one. Even at 20 ms settle the source produced only +3.5 dB, when Focus reads
the same transmitter at -69 to -75 dBm against a -100 floor. Candidate
explanations, none yet tested: the source is on air for only part of each
lap's 3 ms visit to bin 66, so a median across three laps washes it out; the
firing pipeline's ~1.5 s bridge latency makes "source on during this specific
3 ms" hard to guarantee; and the `FULL begin()` row is internally odd, reading
a *lower* floor (-112.3) than the settled light path despite skipping the
settle only because a full begin already takes longer.

Two earlier attempts at this measurement were invalid for the same reason and
are recorded so the next one does not repeat them: firing only for a lap's
duration radiates entirely after the lap ends, and reads as a silent band in
every arm.

## Next

- Establish the floor result properly: more laps, both link levels, and a
  settle sweep finer than 0/5/20 ms, to find where the reading stabilises.
- Only then ask the detection question, with the source's presence during the
  bin visit *verified* rather than assumed — which needs a device-side
  timestamp, not a host-side one.
- Decide separately whether the margin constant should be recalibrated against
  settled readings, or whether Pass A should settle before sampling. Those are
  different fixes with different costs to lap time.


---

# Established: the floor stabilises by 3 ms, at ~15% of lap time

**Raw:** `private/ws17-floor-20260905T181609Z.jsonl`. Eight settle values,
five laps each, five bins, **transmitter idle throughout**. Silence is the
point: the floor is a property of the receiver and its retune, so measuring it
with no source removes the timing problems that invalidated the first attempts
at the detection question.

| settle | lap time | bin 0 | bin 20 | bin 40 | bin 66 | bin 84 |
|---|---|---|---|---|---|---|
| **0 ms (shipped)** | 1.70 s | -112.2 | -115.1 | -111.6 | -115.0 | -113.6 |
| 1 ms | 1.62 s | -105.3 | -108.4 | -104.2 | -108.5 | -107.0 |
| 2 ms | 3.76 s | -102.8 | -102.3 | -95.1 | -102.0 | -98.8 |
| **3 ms** | 1.74 s | -102.1 | -101.5 | -94.5 | -101.9 | -98.5 |
| 5 ms | 2.01 s | -102.7 | -102.0 | -94.5 | -102.7 | -99.3 |
| 10 ms | 2.10 s | -102.5 | -101.9 | -94.0 | -102.2 | -98.6 |
| 20 ms | 4.13 s | -102.0 | -101.8 | -94.5 | -102.3 | -98.3 |
| 40 ms | 5.60 s | -102.7 | -102.6 | -94.0 | -102.3 | -98.7 |

**Three milliseconds is enough.** Readings are flat from 3 ms through 40 ms.
The shipped 0 ms configuration under-reads by 9.5 dB (bin 0) to 17.6 dB
(bin 40), and 1 ms recovers only about half of it.

**The cost is small.** A settle is paid once per bin, so 85 bins multiply it:
3 ms predicts 0.26 s added per lap, and the measured 1.70 -> 1.74 s is that
within noise. 40 ms costs 5.60 s, which is why this is worth pinning down
rather than picking a generous value. Roughly 15% of lap time buys 10-18 dB of
floor accuracy. (The 2 ms and 20 ms lap times are out of trend and are
believed to be timing noise in the host's lap measurement, which includes
status polling; they do not affect the floor readings.)

## A prediction that was wrong, and what it means

This measurement was designed with bin 0 as a control: bin 0 takes a full
`begin()` while later bins take the light retune, so if the under-read were a
settle artifact, bin 0 should not move. **It moved 9.5 dB.** Re-reading
`performEnergySweep()`, the settle is skipped only when the *global* FULL
retune mode is on — the condition is `!benchSweepRetuneFullEveryBin()`, not a
per-bin test — so in the shipped LIGHT mode bin 0 receives the full begin
*and* the settle. It was never a control.

Bin 0 did shift least (+9.5 dB against +12.5 to +17.6 elsewhere), which is
consistent with a full begin providing partial settling on its own, but that
is an observation and not a demonstrated mechanism. The clean version of this
control would compare FULL mode against LIGHT+3 ms across all bins.

## Also unexplained

The settled floor is not flat across the band: bin 40 reads about 8 dB above
bins 20 and 66 at every settle value from 2 ms up. That could be real ambient
energy near 912 MHz or a front-end response, and this run does not
distinguish them. It matters because Pass A compares each bin against a floor
plus a margin, and a band-dependent floor is a different situation from a flat
one.

## What follows

- A ~3 ms settle in Pass A is cheap and recovers most of the error. It is the
  obvious fix, but it cannot be made alone: the shipped margin constant was
  calibrated against under-read values, so changing the settle without
  recalibrating the margin changes Sweep's peak decisions in an untested
  direction.
- This is one environment and one link. The floor is a receiver property so it
  should transfer, but it has not been checked at a second location.
- The detection question — can Pass A flag a bin carrying traffic — remains
  open and is now better approached with real repeater traffic on two known
  channels (bins 34 and 66), using the other 83 bins as within-run controls,
  than with a fixture whose presence during a 3 ms visit cannot be guaranteed.


---

# The under-read is not cosmetic: Pass A misses real traffic because of it

**Raw:** `private/ws17-traffic-20260905T182652Z.jsonl`. 68 laps, **fully
passive — this run transmitted nothing.** The sources are two live repeaters
on known channels; the bench transmitter was explicitly quieted. Settle
alternated 0 ms and 3 ms lap by lap so both configurations saw the same
traffic. Ended early at lap 68 of 200 on a USB-CDC transport timeout: this
script lacks the retry hardening the Focus matrix runner has.

Bins 34 (MeshCore, 910.525 MHz) and 66 (MeshOregon, 918.5 MHz) carry traffic;
seven bins spread across the band are read in the *same lap* as controls, so
the comparison needs no external ground truth.

| | control median | control max | bin 34 max | bin 66 max |
|---|---|---|---|---|
| 0 ms (shipped), 34 laps | -115.2 | -112.3 | **-95.3** | **-113.8** |
| 3 ms, 34 laps | -102.8 | -96.4 | **-56.5** | **-50.8** |

Laps where a traffic bin exceeded the control maximum: 2/34 and 0/34 at 0 ms;
2/34 and 2/34 at 3 ms. Adjacent bins (33/35/65/67) were clean at 0/34 except
one 1/34 excursion at bin 67, so energy lands in the intended bin.

## Pass A's own threshold would reject what it caught

`ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10` is 350, i.e. a bin must sit **35 dB**
above the noise floor to be flagged as a peak. Applying that to what was
actually measured:

| | floor | best traffic reading | excursion | flagged? |
|---|---|---|---|---|
| **0 ms (shipped)** | -115.2 | bin 34 at -95.3 | **19.9 dB** | **no** |
| | | bin 66 at -113.8 | 1.4 dB | no |
| **3 ms** | -102.8 | bin 34 at -56.5 | **46.3 dB** | yes |
| | | bin 66 at -50.8 | 52.0 dB | yes |

**As shipped, Pass A would not have flagged either repeater in this run. With
a 3 ms settle it would have flagged both.** That is the same traffic on
alternating laps, so it is not a difference in conditions.

This is what makes the settle a defect rather than a calibration curiosity.
The floor measurement showed the noise floor read 9.5-17.6 dB low; this shows
the *signal* is under-read far more — the same repeater burst read -95.3 dBm
at 0 ms and -56.5 dBm at 3 ms, a 39 dB difference — which matches
`version.h`'s note that the under-read grows with signal strength. A floor
that is 13 dB low and a signal that is 39 dB low do not cancel: the excursion
that the margin tests collapses from 46 dB to 20 dB, and falls under the
threshold.

## What this does and does not support

Supported: at the shipped settle, real traffic that Pass A physically sampled
did not reach its own flagging threshold, and a 3 ms settle fixed that in the
same run. The mechanism is consistent across the floor sweep, this run, and
the prior Cell finding.

Not supported: any rate claim. Two detections in 34 laps per configuration is
a tiny count, the hit rate is limited by the ~3 ms bin visit coinciding with a
burst rather than by sensitivity, and 0 ms versus 3 ms hit counts (2 and 0
against 2 and 2) are not distinguishable at that size. **The decisive evidence
here is the magnitude of the excursion, not the number of hits.** Traffic was
also uncontrolled; alternating laps controls for drift but not for a burst
happening to fall in one arm.

Also unchanged: one environment, one link, one repeater pair.

## Recommendation

Pass A should settle before sampling, and the margin must be re-derived in the
same change. 3 ms costs about 0.26 s of a 1.7 s lap. Shipping the settle alone
would leave a 35 dB margin calibrated against under-read values now being
applied to correctly-read ones, which changes peak decisions in an untested
direction — the fix is one change, not two.


---

# Confirmation at 300 laps

**Raw:** `private/ws17-traffic2-20260905T231056Z.jsonl`. 300 laps, **0 skipped**,
62.7 minutes, fully passive — the bench transmitter was quieted and this run
transmitted nothing. Same two live repeaters, same alternating 0/3 ms settle,
same within-lap controls. The harness gained retry-on-transport-failure and
skip-a-lap-rather-than-abort after the previous attempt died at lap 68.

Judged by Sweep's own rule: a bin is flagged when it sits 35 dB
(`ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10`) above the lap's noise floor.

| | bin 34 (MeshCore) | bin 66 (MeshOregon) | adjacent + control bins | best traffic excursion |
|---|---|---|---|---|
| **0 ms (shipped)** | 0/150, CI [0.000, 0.025] | 0/150, CI [0.000, 0.025] | **0 flagged** | **19.0 dB** |
| **3 ms** | 3/150, CI [0.007, 0.057] | 3/150, CI [0.007, 0.057] | **0 flagged** | **51.6 dB** |

## What this establishes

**The shipped configuration never came close to flagging traffic it was
physically sampling.** Its best excursion across 150 laps was 19.0 dB against
a 35 dB threshold — not a near miss, a factor of two in dB terms. The settled
configuration reached 51.6 dB on the same traffic in interleaved laps.

**Specificity is clean.** Across both arms, no adjacent bin (33/35/65/67) and
no control bin (5/15/25/45/55/75/80) was ever flagged — roughly 1,650
non-traffic bin observations per arm with zero false flags. When the settled
configuration flags something, it flags the two channels that actually carry
traffic and nothing else.

**Both repeaters behaved alike at 3 ms** (3/150 each), where the shorter
earlier run had seen MeshOregon twice and MeshCore twice. The per-session
difference in activity the operator described shows up as variance between
runs rather than as a systematic difference here.

## What this does not establish

**The flag *rates* are still not statistically separated.** 0/150 gives
[0.000, 0.025] and 3/150 gives [0.007, 0.057]; those overlap on [0.007,
0.025]. Separating a 0% rate from a 2% one needs roughly 300 laps per arm, not
150, and this run was sized before that arithmetic was done.

That gap does not weaken the conclusion, because the rate is a *consequence*
of the margin arithmetic rather than independent evidence for it: a
configuration whose best excursion is 19.0 dB cannot flag a 35 dB threshold at
any sample size. The rate would only become the load-bearing evidence if the
excursion result were ambiguous, and it is not.

**The ~2% flag rate at 3 ms is a coincidence rate, not a sensitivity figure.**
Pass A visits each bin for about 3 ms per lap, so it can only flag traffic
that happens to be transmitting during that window. That bound applies equally
to a fixed Sweep and is a property of the design, not of the settle.

## Status

This closes Workstream 17's entry question. Pass A's settle under-read is not a
calibration curiosity: it is the difference between flagging the two busiest
channels in the band and flagging nothing at all. The fix — settle, and
re-derive the margin in the same change — remains unapplied and untested.
