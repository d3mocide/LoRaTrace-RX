# Workstream 12 — repeated Focus passes are deterministic; the cost is the dwell floor

**Build:** bench image, `V=1.1.0-beta;R=2256a30;BENCH=1`. **Date:** 2026-09-06.
**Setup:** non-transmitting. Profile forced to Meshtastic so the bench image's
pinned home applied (918.500 MHz, SF8/BW125 — a device left on MeshCore surveys
at a different bandwidth, and Focus receives at the *home* bandwidth). Survey
target US Sweep bin 43. Ambient only; no source was armed at any point.
**Fixture:** `scripts/phase12_coverage_campaign.py`.
**Raw:** `private/2026-09-06-phase12-coverage-campaign.jsonl` (80 passes).

This is the measurement the `sampled`/`repeated` coverage thresholds were
waiting on — the one W12 item no single-pass matrix could supply, because
coverage is defined over *repeated* requests. It characterises repeatability,
not detection; detection was measured in the
[2026-09-04 matrix](2026-09-04-phase12-focus-matrix.md) and is not what a
coverage label reports.

## Result

20 passes at each of four dwells. **80/80 completed with home restore; zero
timeouts, zero failures, zero non-complete rows.**

| Dwell | Valid | Observation ms (median / stdev) | Samples (min–max) | Away ms |
|---|---|---|---|---|
| 250 ms | 20/20 | 250.0 / **0.0** | **14 – 14** | 324 |
| 500 ms | 20/20 | 500.0 / **0.0** | **26 – 26** | 574 |
| 1000 ms | 20/20 | 1000.0 / **0.0** | **51 – 51** | 1074 |
| 2000 ms | 20/20 | 2000.0 / **0.0** | **101 – 101** | 2074 |

Sample counts match `focusSamplesForDwell()` exactly at every dwell, and the
min and max are the same number: a pass's yield does not vary across repeats
at all.

## What it settles

**1. Yield holds across repeats — completely.** The campaign was looking for
whether pass 7 of 20 returns materially fewer accepted samples than pass 1. It
does not: observation time stdev is 0.0 ms and sample count is invariant. So
accumulated observation time is a sound basis for a coverage threshold, and —
because passes × dwell reproduces total observation time exactly — **a
threshold in accumulated time and a threshold in valid passes are the same
statement.** Whichever is easier to explain to an operator is the right one to
ship; the measurement does not prefer either.

**2. Per-pass overhead is a flat 74 ms, independently.** Away time minus
observation time is 74 ms at every dwell, confirming the design entry's
"dwell + ~73 ms" from a different fixture. It is fixed cost, not proportional.

**3. That flat cost is the argument for a dwell floor.** Because the overhead
does not scale, short passes buy less observation per unit of Watch time:

| Dwell | Observed / away | Passes for 10 s observed | Radio-away to get there |
|---|---|---|---|
| 250 ms | 77.2% | 40 | 13.0 s |
| 500 ms | 87.1% | 20 | 11.5 s |
| 1000 ms | 93.1% | 10 | 10.7 s |
| 2000 ms | 96.4% | 5 | **10.4 s** |

Reaching the same 10 s of observation costs 25% more Watch time at 250 ms than
at 2000 ms. Combined with §6.4's finding that a 100 ms pass cannot both catch a
source and reject ambient at any threshold, this says `sampled` wants a
**minimum dwell**, not only a minimum accumulated time: short passes are worse
on both axes at once, and no amount of repeating them fixes either.

## What it does not settle

**It does not choose the thresholds.** Choosing what an operator should be told
counts as "covered" is a judgement, and this supplies its input. What the
measurement removes is the hard part — with yield deterministic, the choice is
no longer contingent on how repeatable a pass is.

**It says nothing about detection.** Ambient only, one bin, one link, one
location. Three of 80 passes returned a nonzero count above their own median
(values 1 and 2), and `C6 >= 2` fired on exactly one — 1/80, consistent with
the 1.7% upper bound the matrix reported and with no source present. That is a
false-positive observation on quiet ambient, not a detection rate.

**It does not revisit the activity decision.** Focus reports coverage and never
activity ([design entry §8](../research/phase12-survey-truth-design.md)); this
measurement is about how much looking happened, which is all a coverage label
is permitted to describe.
