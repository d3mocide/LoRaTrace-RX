# Phase 9 Sweep Pass B — design research

**Status:** implemented and hardware-verified (2026-08-28), including a
same-day timing revision. Initial version deferred all of Pass B until
after Pass A's full 221-bin loop finished, selecting the K=8 *strongest*
peaks from the whole sweep. An operator observation caught a real
architectural gap in that: a deferred pass can arrive well after a brief
transmitter has gone quiet again. Revised to run Pass B **immediately**
when Pass A finds each peak, capped at the first 8 peaks encountered
(no longer "strongest" — see "Timing revision" below). `energy.csv` was
pulled off the SD card and inspected per-row afterward, which corrected an
earlier guess: the standing CAD-at-arbitrary-bin unknown looks driven by
spreading factor (a false-positive rate that scales with CAD's dwell time
at a fixed 2-symbol window), not sync word as first suspected — see
"Hardware verification." A proper 400-cycle controlled matrix
(`BENCH_PASS_B_CAD`, `scripts/phase9_pass_b_cad_bench.py`, 2026-08-29) then
ran against a real quiet/pulse control, and the SF-only story didn't fully
survive that either — see "Controlled bench matrix" below. The one
genuinely clean, unambiguous result: the sourced table's exact match to a
real transmission (SF8/BW125) scored 0/20 quiet false positives and 20/20
pulse detections. A same-day interleaved-order repeat (`--order
interleaved`, now the script's default) confirmed the exact-match result
again and mostly dissolved the earlier SF-vs-time confound — see
"Interleaved re-run" below — leaving one genuine residual: a mild
elapsed-session-time drift in quiet false positives that survives combo
randomization, so it isn't purely an SF artifact. A shielded-box quiet
control (2026-08-29, "Shielded-box quiet control" below) then ran the same
matrix two more ways — both radios in one foil-lined box, then the
Cardputer alone with the Heltec outside — and found shielding raised the
quiet false-positive rate rather than lowering it (10.5% open-room →
17.5% both-shielded → 14.5% Cardputer-only), most likely because a small
reflective enclosure traps a device's own RF emissions and reflects them
back into its own receiver rather than letting them dissipate the way open
air does. The elapsed-session drift did not replicate cleanly in either
shielded run, weakening confidence it was a strong, reproducible effect
in the first place. Pooled across all three 400-cycle runs (1,200 cycles
total), the two combos with genuinely reproducible behavior are SF8/BW125
(2/60 quiet, still reliably 20/20 on the real pulse in every run) and
SF11/BW500 (22/60 quiet, consistently the noisiest, still 19-20/20 on the
real pulse); the other eight combos vary too much run-to-run at n=20 per
condition to characterize confidently yet. Per-combo confidence weighting
(`PassBConfidence`, `pass_b_plan.h`) then shipped as a descriptive
`energy.csv` column derived from that pooled data — see "Confidence
weighting" below.

## What Pass B is

`research/LoRaTrace-Phases-7-10-Design.md` §7.2 (authoritative spec):

> **Pass B — LoRa likelihood:** run CAD at a small sourced set of SF/BW
> combinations only for energy peaks, operator-selected bins, or a
> scheduled sparse subset. This avoids the slow and fragile product of
> every frequency × SF × bandwidth.
>
> A CAD hit away from a known Meshtastic or MeshCore channel is labeled
> **unknown LoRa candidate**, not Reticulum. Protocol attribution requires
> evidence the radio layer cannot provide.

ROADMAP.md's Phase 9 entry restates this as part of the *deliverable*, not
an optional extra — Pass A alone doesn't close Phase 9.

## What already exists, ready to reuse

- **Pass A (energy)** is complete and hardware-verified: `performEnergySweep()`
  (`radio_task.cpp`) retunes all 221 bins, takes 4 `getRSSI(false)` samples
  each, and logs threshold-filtered peaks to `energy.csv`
  (`EnergyObservation`, `energy_observation.h`).
- **`EnergyObservation` is already forward-compatible with Pass B.** Its
  `sf`/`cr_denom`/`sync_word` fields exist today with a documented "0 = no
  CAD attempted" sentinel, and `packet_metadata_present` is already a field
  (always `false` until Pass B lands). No schema change needed there beyond
  populating those fields and adding CAD result values to
  `EnergyObservationResult` (currently only `ENERGY_PEAK`/`RADIO_ERROR` —
  the enum's own comment: *"Pass-B CAD_FREE/CAD_DETECTED/CAD_TIMEOUT values
  are deliberately NOT added yet... SX1262 CAD-at-arbitrary-bin behavior
  isn't verified"*).
- **The CAD mechanics themselves are proven**, just not yet at an arbitrary
  bin. Probe's `performDiscoverySweep()` already does exactly the CAD +
  bounded-receive-on-hit sequence Pass B needs, at a curated list of known
  channels: `radio.startChannelScan()` with a `CadParams` config
  (`DISCOVERY_CAD_TIMEOUT_MS = 300`), `RADIOLIB_LORA_DETECTED` vs
  `RADIOLIB_CHANNEL_FREE`, then on a hit, `radio.startReceive()` for a
  bounded window and `readDetectionLocked()` to promote a real packet to
  `Detection`. Pass B's implementation is "the same sequence, at Pass A's
  peak bins instead of the curated candidate list" almost verbatim.

## Two real forks found while reading the code

### 1. Detection's classification has no "unknown LoRa candidate" value yet — this is a correctness gap, not a style question

The energy_observation.h comment says Pass B's receive-on-hit will
"produce a separate Detection row exactly like Probe's own receive-on-hit
does." But `detection.h`'s actual classification logic today is:

```cpp
inline const char *detectionClassification(const Detection &det) {
    return missionProfileName(det.profile);
}
```

— it can only ever return the *mission profile name* (its own comment
admits this is a Phase 2 placeholder: *"Real post-hoc classification...
replaces this via fingerprint.h"*, still `[ ]` unbuilt per CLAUDE.md's file
layout). Sweep only runs under `RETICULUM`/`GENERAL_EXPLORATION`
(`discoveryPlanForProfile()`'s own fallback confirms these two profiles
have no curated channel table — Sweep is their only tool). So if Pass B
just calls `enqueueDetection()` with `activeProfile` the way Probe does,
every Pass B hit's CSV `classification` column would read `RETICULUM` —
**exactly the mislabeling DESIGN.md §7.2 explicitly forbids** ("not
Reticulum... protocol attribution requires evidence the radio layer cannot
provide").

This means Pass B cannot reuse Probe's receive-on-hit path unmodified and
be correct. It needs one small, targeted change: a way for a `Detection`
row to say "unknown LoRa candidate" instead of a profile name. Proposed:
add a bool (e.g. `off_grid`) to `Detection`, set `true` only by Pass B's
own enqueue path, and change `detectionClassification()` to:

```cpp
inline const char *detectionClassification(const Detection &det) {
    if (det.off_grid) return "unknown_lora_candidate";
    return missionProfileName(det.profile);
}
```

`LOG_CSV_HEADER`/`detectionFormatCsv()` don't need a new column for this —
`classification` is already a rendered column, this only changes what one
function returns for it. The `test_detection` comma-count test
(`commas(LOG_CSV_HEADER) == commas(row)`) is unaffected since no column is
added. This is a small, surgical change, but it's a real one — not
optional polish. **This needs your sign-off before I touch `detection.h`,
since it changes a stable, already-shipped record's semantics, even though
the shape stays byte-identical.**

### 2. The literal "every peak, every SF/BW combo" reading doesn't bound

Deduplicating the SF/BW pairs already established as real, sourced
upstream values across `discovery_plan.h`/`channel_plans.h` (not inventing
new numbers) gives exactly 10 distinct combos:

| SF | BW (kHz) | Sourced from |
|---|---|---|
| 7 | 62.5 | MeshCore US Narrow (current default) |
| 7 | 250 | Meshtastic ShortFast |
| 8 | 125 | MeshOregon (operator local physical tuple) |
| 8 | 250 | Meshtastic ShortSlow |
| 9 | 250 | Meshtastic MediumFast |
| 10 | 250 | Meshtastic MediumSlow / MeshCore upstream default |
| 11 | 125 | Meshtastic LongModerate |
| 11 | 250 | Meshtastic LongFast |
| 11 | 500 | Meshtastic LongTurbo |
| 12 | 125 | Meshtastic LongSlow |

Running all 10 at every Pass-A peak, at Probe's existing 300ms CAD
timeout, is 3 seconds of home-away time *per peak bin*. DESIGN.md's own
words are "avoids the slow and fragile product of every frequency × SF ×
bandwidth" — so an unbounded "all combos × all peaks" reading is exactly
what it says not to build. The design doc's own phrasing offers the
bound: **"only for energy peaks, operator-selected bins, or a scheduled
sparse subset"** — three ways to bound it, not a mandate to hit every peak
with every combo.

**Proposed bound:** cap Pass B to the top-K strongest peaks from Pass A
(by `rssi_peak_dbm_x10`), K chosen so worst case (K × 10 combos × 300ms)
stays a small, clearly-stated fraction of Pass A's own ~10s runtime — e.g.
K=8 gives a 24s worst-case Pass B addition, roughly 2-3x Pass A's own
duration, still bounded and stated up front rather than proportional to
how "loud" the room is. Exact K is a tuning question for the hardware
bench phase (same as CAD symNum and the sweep margin both needed a real
bench matrix before their numbers were trusted), not something to freeze
from a desk calculation.

## The standing hardware unknown (already flagged in the code, not new)

`energy_observation.h`'s comment is explicit: *"SX1262 CAD-at-arbitrary-bin
behavior isn't verified."* Every existing CAD calibration
result (Phase 8's symNum matrix, Phase 9's margin bench) was measured at
**curated candidate frequencies** — real channel centers a LoRa radio is
actually built to be tuned to. Pass B's bins are Pass-A peak *centers on a
uniform 250kHz grid*, which won't generally land on a real channel center.
Whether CAD behaves the same (timeout rate, false/miss behavior) when
retuned to an arbitrary off-grid frequency is a genuine open question, not
an implementation detail — it needs the same kind of real bench matrix
Phase 8's CAD work and Phase 9's margin calibration both already used
(`scripts/phase8_cad_rate_bench.py`, `scripts/phase9_sweep_margin_bench.py`
are the existing pattern to extend, not reinvent).

## Proposed scope for a first implementation slice

1. `detection.h`: add `off_grid` bool to `Detection`, branch
   `detectionClassification()` on it. Update `test_detection` fixtures.
2. `energy_observation.h`: add `CAD_FREE`/`CAD_DETECTED`/`CAD_TIMEOUT` to
   `EnergyObservationResult`. Update `test_energy_observation`.
3. A small sourced `PASS_B_SF_BW_CANDIDATES[]` table (the 10-row table
   above), mirroring `discovery_plan.h`'s own "pure fixed-data layer"
   convention — a new file or a section of `energy_plan.h`, TBD by
   whichever keeps `energy_plan.h`'s existing "pure formula, not a curated
   list" framing honest (leaning toward a new small file, since this *is*
   a curated list, the thing `energy_plan.h`'s own top comment says it
   deliberately isn't).
4. `radio_task.cpp`: after Pass A completes in `performEnergySweep()`,
   select the top-K peak bins, then for each run CAD across the sourced
   SF/BW table at that bin's frequency (reusing `DISCOVERY_CAD_TIMEOUT_MS`
   and the existing bounded-receive-on-hit sequence verbatim), logging
   each attempt as an `EnergyObservation` row and promoting a real hit to
   `Detection` with `off_grid = true`.
5. Host tests for the new table/enum/classification branch.
6. **Before trusting any result:** a hardware bench matrix answering the
   standing CAD-at-arbitrary-bin question above, following the same
   pattern as the existing CAD-rate and sweep-margin benches.

No new on-device menu item: this is additive richness inside the existing
`S`/Sweep trigger (DESIGN.md's own framing is one "two-pass acquisition"
feature, not two independently-toggleable ones), so CLAUDE.md's "new
operator-facing behavior gets a menu toggle" house rule doesn't add a new
row here — Sweep already has its trigger.

## What actually landed

Items 1-5 above, plus telemetry that wasn't originally scoped but turned
out to be needed to observe the engine at all during verification:
`radioPassBAttemptCount()`/`radioPassBDetectionCount()` (cumulative,
`radio_task.h`/`.cpp`) and `STATUS`'s new trailing `;PBA=<n>;PBD=<n>`
fields (`serial_control.cpp`). `PASS_B_SF_BW_CANDIDATES` landed as its own
`src/pass_b_plan.h` (new file, per the leaning in item 3) with a
`PASS_B_CR_DENOM_PLACEHOLDER`/`PASS_B_SYNC_WORD_PLACEHOLDER` pair (CR 4/5,
RadioLib's stock 0x12 sync word) — explicitly flagged in that file's own
comment as a first placeholder, not a verified choice, since whether CAD
is sync-word-sensitive on this hardware is exactly the standing unknown
below.

`waitForDioUntil()` (`radio_task.cpp`) gained a second parameter — an
abort-check function pointer, defaulting to `discoveryAbortPending` so
Probe's two existing call sites are untouched. Pass B passes
`energyAbortPending` explicitly. Without this, a Sweep-cancel request
during Pass B's own CAD/RX waits would have gone unnoticed until the next
full timeout, since the un-parameterized version only ever checked Probe's
own cancel flag.

Host tests: `test_detection` (+1: off-grid classification), `test_energy_observation`
(+1: Pass B result names), new `test_pass_b_plan` (3 tests: table size/
bounds, no duplicate SF/BW pairs, bound matches the design doc). Native
suite: 142/142. Both `cardputer-adv` and `cardputer-adv-bench` build
clean.

## Timing revision: immediate, not deferred (same day, after operator review)

The paragraph above describes the *first* version: Pass B ran only after
Pass A's entire 221-bin loop finished, selecting the `PASS_B_TOP_K_PEAKS`
(then named for exactly this) *strongest* peaks from the whole sweep by
`rssi_peak_dbm_x10`.

The operator asked, after seeing the first hardware run's `PBD=0` result:
*"is Pass B a deep dive into the captured traffic... something I noticed
is that we might pick up a brief packet and miss it in pass B"* — and this
is exactly right. Pass A never captures packets, only RSSI; Pass B goes
back to a flagged frequency hoping something is still transmitting there.
With the deferred design, a peak found early in Pass A's ~9-second loop
might not get its Pass B follow-up until Pass A fully finishes *and* Pass
B works through however many other peaks come first — potentially 10+
seconds later, or never, if it wasn't among the K strongest. For a brief,
one-shot burst, that gap is often long enough for the transmitter to be
gone.

Revised: Pass B now runs **immediately** inside Pass A's own loop, the
moment a bin is flagged as a peak, before advancing to the next bin. The
K=8 bound stays (same worst-case cost), but it's no longer "strongest" —
it can't be, since future peaks aren't known yet when deciding whether to
spend time on the current one. Renamed `PASS_B_TOP_K_PEAKS` →
`PASS_B_MAX_PEAKS_PER_SWEEP` to stop the name claiming a ranking that no
longer happens; it's now "first 8 peaks encountered this sweep," which is
arguably more aligned with the actual goal than ranking by loudness ever
was — signal strength doesn't predict whether a transmitter is still on
air, only recency does.

Mechanically: extracted the old inline Pass-B block into its own
`passBCadAtBin(bin, freq, aborted, failed)` function, removed the
top-K-by-RSSI tracking array entirely, and replaced it with one
`passBPeaksThisSweep` counter incremented at the point Pass A finds each
peak, calling `passBCadAtBin()` right there. Pass A's own loop now also
checks `if (aborted || failed) break;` after that call, since Pass B can
now fail/abort mid-bin and the outer loop needs to notice.

## Hardware verification (real devices, both attached)

**First version (deferred, strongest-K):** quiet room completed in 8.8s
with zero Pass-A peaks, so Pass B correctly did zero work (`PBA=0`). With
the Heltec's `BEACON` mode (its own LONG_FAST candidate — SF11/BW250/CR5,
Meshtastic sync 0x2B) pulsing every 2s as a real RF stimulus, Pass A found
4 peaks and Pass B ran exactly 4 × 10 = 40 CAD attempts (`PBA` 0→40),
completing in 44.6s total. Home correctly restored, no crash/hang/
watchdog reset — the core engineering validation that slice needed.

**Revised version (immediate), same beacon test repeated:** `WI` visibly
*paused* at bin 175 for several polls while `PBA` climbed 0→9→10 — Pass B
ran right there, before Pass A moved to bin 176, confirming the
interleaving actually interleaves rather than just changing bookkeeping.
Only 1 peak was found this run (real async RF vs. a systematically
retuning receiver — expected run-to-run variance, not a regression), so
only 10 CAD attempts ran and the whole sweep completed in 12.1s. Home
restored correctly again (`F=918500`, `SD=1`), no crash.

**`PBD` stayed 0 both times**, and at the time this looked like it might
be a sync-word story (Pass B's placeholder 0x12 vs. the beacon's actual
0x2B). **Correction, after actually pulling and reading `energy.csv` off
the SD card** (both runs' full files, `run0064`/`run0065`): the sync-word
theory doesn't hold up against the real per-attempt data, and something
more specific and better-evidenced does.

**`CAD_DETECTED` rate tracks spreading factor almost perfectly, not
whether the combo matches the beacon:**

| SF | Detected / attempted (both runs combined) |
|---|---|
| 7 | 0/10 (0%) |
| 8 | 0/10 (0%) |
| 9 | 3/5 (60%) |
| 10 | 3/5 (60%) |
| 11 | 5/15 (33%) |
| 12 | 3/5 (60%) |

SF7 and SF8 never once detected anything, across every peak in both runs.
SF9 and up detected a third to two-thirds of the time — including firing
at combos sharing nothing with the beacon (SF12/BW125, on the single peak
in the second run, where SF9-11 all came back free) and *missing* at the
beacon's own exact shape (SF11/BW250 detected in only 1 of 4 attempts
across `run0064`'s four peaks). That inconsistency — detecting the wrong
shape more often than the right one — is the signature of a false-positive
rate driven by symbol duration, not a real correlation to the transmitter.
LoRa symbol time is `2^SF / BW`: at a fixed 2-symbol CAD window
(`discoveryCadSymbolConfig()`'s default), SF7 gets under 2ms of actual
signal correlation per attempt while SF12 gets tens of milliseconds — far
more opportunity to false-trigger on ambient energy. Pass B only ever
runs at bins Pass A already flagged as elevated-RSSI peaks, which is
exactly the population most prone to this at long dwell times.

This is not a new problem surfacing — it's the **same "CAD symNum
tuning... real false-positive/miss tradeoff needs bench testing" item
DESIGN.md §7 has carried as open since Phase 0**, now with a clean,
quantified illustration inside Pass B specifically. Sync word may still
matter for the final decode step (the one true-shape `CAD_DETECTED` still
didn't promote a packet), but that's a single data point next to this
SF-driven pattern, not the dominant explanation the first-pass guess
assumed.

**Practical implication:** Pass B today treats every combo's
`CAD_DETECTED` as equally meaningful. This data says that's wrong — a
high-SF `CAD_DETECTED` at a Pass-A peak carries much less evidentiary
weight than a low-SF one, purely from dwell-time false-positive exposure,
independent of whether real LoRa traffic is present. Worth a per-SF (or
scaled-symNum) confidence treatment in a future slice, calibrated the
same way Phase 8's CAD symNum work and Phase 9's sweep-margin work both
already were — a real bench matrix with a known-quiet control, not a
desk estimate.

## Controlled bench matrix (2026-08-29): `BENCH_PASS_B_CAD`

The SF-only hypothesis above was built from two small, uncontrolled
samples (4 peaks + 1 peak, from real sweeps, all under one pulsing
condition). Testing it properly needed a real quiet-vs-pulse control per
combo — which production Pass B can't provide, since it only ever runs at
a bin Pass A has already flagged as loud, so a genuinely quiet room never
exercises it at all.

Added a new bench-only opcode, `BENCH_PASS_B_CAD` (`bench_fault.h`'s
`benchPassBCadTriggerAllowed()` gate, `radio_task.cpp`'s
`performBenchPassBCadTrigger()`), that runs one Pass B CAD attempt on
demand at a fixed test frequency (918.5MHz, the same MESH_OREGON point
`phase8_cad_rate_bench.py` already uses) for an operator-chosen
`PASS_B_SF_BW_CANDIDATES` row — independent of any real peak. Extracted
the single-combo CAD sequence out of `passBCadAtBin()`'s loop into its own
`passBCadOneCombo()` so both the bench trigger and production Pass B share
exactly one implementation. Logs through the same `enqueuePassBObservation()`
path, so results land in `energy.csv` exactly like production Pass B's own
rows — this script doesn't read that CSV itself (no file access over
Serial Control by design); the SD card still has to be pulled afterward.
Production firmware rejects the opcode (verified: `ERROR UNSUPPORTED`
before even flashing the bench image); `cardputer-adv-bench` accepts it.

`scripts/phase9_pass_b_cad_bench.py` ran 20 quiet + 20 pulse cycles per
combo (400 total), configuring the Heltec to its own `MESH_OREGON`
candidate (SF8/BW125/CR5, Meshtastic sync 0x2B — the *same* physical
tuple Pass B's own table sources its SF8/125 row from) and firing one
`ARM 0` pulse per pulse-cycle, mirroring `phase8_cad_bench.py`'s exact
convention. Completed cleanly: `PBA` reached exactly 400, clean home
restore throughout, no crashes. (One earlier attempt hit a single dropped
response over native USB partway through — a known, already-documented
transport characteristic, not a firmware bug; the request had in fact
completed on-device, only its reply was lost. Bumped the trigger's retry
timeout and re-ran clean.)

| Combo | Quiet detected/20 | Pulse detected/20 |
|---|---|---|
| SF7/BW62.5 | 0 | 1 |
| SF7/BW250 | 1 | 0 |
| **SF8/BW125 (exact match)** | **0** | **20** |
| SF8/BW250 | 2 | 2 |
| SF9/BW250 | 5 | 20 |
| SF10/BW250 | 3 | 20 |
| SF11/BW125 | 3 | 0 |
| SF11/BW250 | 5 | 1 |
| SF11/BW500 | 7 | 20 |
| SF12/BW125 | 5 | 0 |

**The one clean, unambiguous result:** the table's exact match to the real
transmission — SF8/BW125, sourced from the same MeshOregon tuple the
Heltec was configured to — scored a perfect 0/20 quiet false positives and
20/20 pulse detections. That's real, solid evidence the underlying CAD
mechanism works correctly and reliably when Pass B's combo actually
matches what's transmitting.

**The SF-only story from the smaller sample doesn't fully survive this
controlled run.** Some non-matching combos (SF9/250, SF10/250, SF11/500)
also hit 20/20 on the real pulse — plausibly because their bandwidth is a
superset of the real 125kHz signal, so CAD picks up genuine transmitted
energy as a broadband correlation event even without an exact symbol
match. But others (SF11/125, SF11/250, SF12/125, both SF7 combos, SF8/250)
barely register the real pulse at all (0-2/20) — for these, CAD is
essentially blind to the actual transmission, and whatever they show in
the quiet column is unrelated noise, not a miss rate.

**A real design gap in this run, not yet resolved:** combos were tested in
a fixed order (matching the table's own SF-ascending order), each block
taking a few minutes, so the whole matrix spans the same ~18-minute
session in the same order every time. The quiet-condition counts trend
upward through that span (0,1,0,2 → 5,3,3,5,7,5) — consistent with either
"false positives scale with SF" (the original hypothesis) or "ambient RF
crept up over the session" (a time confound), and this run's fixed
ascending-SF order can't distinguish the two: SF and elapsed time increase
together here. A repeat with **randomized or interleaved combo order**
would separate them properly.

## Interleaved re-run (2026-08-29): separating SF from elapsed time

`scripts/phase9_pass_b_cad_bench.py` gained `--order {interleaved,block}`
(interleaved is now the default; `build_schedule()` round-robins all ten
combos each cycle, reshuffled per round, instead of running combo-by-combo).
Each combo's 20 quiet + 20 pulse samples are now spread evenly across the
whole ~18-minute session instead of clustered in one time block, so combo
identity (and SF) is decorrelated from elapsed time. Re-ran the full
400-cycle matrix this way (`run0074/energy.csv`, row order verified 1:1
against the script's own per-cycle event log — zero misalignments):

| Combo | Quiet detected/20 | Pulse detected/20 |
|---|---|---|
| SF7/BW62.5 | 0 | 1 |
| SF7/BW250 | 1 | 0 |
| **SF8/BW125 (exact match)** | **0** | **20** |
| SF8/BW250 | 1 | 9 |
| SF9/BW250 | 2 | 20 |
| SF10/BW250 | 2 | 20 |
| SF11/BW125 | 1 | 0 |
| SF11/BW250 | 4 | 0 |
| SF11/BW500 | 7 | 19 |
| SF12/BW125 | 3 | 1 |

The exact-match combo replicated perfectly: 0/20 quiet, 20/20 pulse, same
as the block-ordered run. SF11/BW500 also replicated almost exactly (7/20
quiet both times, 20→19 pulse) — that combo's high quiet false-positive
rate looks like a real property of it (plausibly its 500kHz bandwidth
being wide enough to correlate against ordinary broadband noise), not a
time-order artifact. The combos that barely detected the real pulse
before (SF11/125, SF11/250, SF12/125, both SF7 rows) still barely detect
it here, so that part of the picture is also real and not an ordering
artifact. SF8/250 fell somewhere between: 2/20 pulse in the block run
became 9/20 here.

**What did change:** overall quiet false-positive count dropped from
31/200 (block order) to 21/200 (interleaved), and the clean *monotonic*
staircase the block run showed (0,1,0,2,5,3,3,5,7,5, rising steadily with
each successive SF-ascending block) is gone — the interleaved per-combo
counts (0,1,0,1,2,2,1,4,7,3) no longer climb steadily with SF. That's
consistent with the block run's staircase being mostly a time-ordering
artifact rather than a clean SF effect, as suspected.

**What didn't fully resolve:** splitting the 200 quiet-phase rows into
session-time quartiles (in actual run order, combo now randomized within
each quartile) still shows a mild rise: 6% → 8% → 16% → 12% detected. If
the false-positive rate were purely a property of which combo ran, this
should have flattened out completely once combo order stopped tracking
time — it didn't, fully. So there is likely a real, modest elapsed-session
component (self-heating, ambient RF drift, or similar) on top of the
per-combo differences that are now much better isolated. Not chased
further this round — see remaining open items below.

## Shielded-box quiet control (2026-08-29): shielding made the baseline noisier, not cleaner

A physical foil-wrapped enclosure was built and verified against an FM
radio (lost signal entirely — confirmed real external RF attenuation).
Two 400-cycle interleaved matrices then ran with it:

**Both radios sharing one box** (`run0076`): overall quiet false-positive
rate went **up**, 17.5% (35/200) vs. the open-room run's 10.5%, and the
exact-match combo (SF8/BW125) — a clean 0/20 across two independent
open-room runs — picked up its first-ever quiet false positive (1/20).
Quartile pattern was 20%→24%→8%→18%, no longer a steady climb, just
noisier throughout.

**Cardputer alone, Heltec outside** (`run0080`, after the operator
identified and fixed the confound): overall quiet false-positive rate
landed at 14.5% (29/200) — between the open-room and both-shielded
numbers, still above the open room. Quartile pattern: 10%→18%→14%→16%,
again no clean climb.

**Most likely explanation:** a small conductive enclosure is good at
blocking external RF (that's what killed the FM signal) but bad for
anything active *inside* it — a device's own RF emissions (MCU/USB
oscillator harmonics, digital switching noise) reflect off the foil
instead of dissipating via free-space spreading the way they would in an
open room, so the receiver ends up seeing more of its own noise, not
less. This held even with just the Cardputer alone in the box, which
argues the effect is partly the Cardputer coupling with its own
reflections, not only mutual coupling between the two radios (which was
still worse than either — both-radios stayed the noisiest of the three
conditions throughout).

**Pulse detection was not suppressed by the shield, unexpectedly:**
overall pulse detection rate in the Cardputer-only run was 44% (88/200),
with the exact-match and near-match combos (SF8/BW125, SF9/250,
SF10/250, SF11/500) still hitting 20/20 — essentially unchanged from the
open-room baseline. The FM test proved the box blocks ~100MHz; it
apparently does not equally block LoRa's ~918MHz. A hand-wrapped foil
enclosure's seams/gaps are a plausible reason: a gap much smaller than
FM's ~3m wavelength (fully blocking) can still leak at LoRa's ~33cm
wavelength (a shorter wavelength "fits" through a smaller gap), so a
seam that stops FM cold may pass a strong nearby LoRa signal fine — real,
useful information about the box's actual shielding profile, not a
bug in the bench script.

**The original elapsed-session-time drift did not replicate cleanly.**
All three runs (open-room 6%→8%→16%→12%, both-shielded 20%→24%→8%→18%,
Cardputer-only 10%→18%→14%→16%) show a different, non-monotonic
quartile-over-time shape — none matching the others. That weakens
confidence there is one strong, reproducible "warms up over ~18 minutes"
effect at all; it looks more like normal run-to-run variance on a
background rate that is itself fairly volatile at n=50 samples per
quartile, rather than a stable phenomenon worth chasing with a bigger
enclosure or RF-absorptive lining.

**Pooled across all three 400-cycle runs (1,200 cycles, 60 quiet + 60
pulse samples per combo):**

| Combo | Quiet FP (open/both-shielded/card-only) | Quiet total/60 | Pulse (typical) |
|---|---|---|---|
| SF7/BW62.5 | 0/0/0 | 0 | rarely fires (real or noise) |
| SF7/BW250 | 1/0/0 | 1 | rarely fires |
| **SF8/BW125 (exact match)** | 0/1/1 | **2** | **20/20 every run** |
| SF8/BW250 | 1/2/1 | 4 | inconsistent (2-9/20) |
| SF9/250 | 2/4/4 | 10 | 20/20 every run |
| SF10/250 | 2/5/4 | 11 | 20/20 every run |
| SF11/125 | 1/8/4 | 13 | rarely fires (0/20 every run) |
| SF11/250 | 4/5/1 | 10 | rarely fires (0-5/20) |
| SF11/500 | 7/8/7 | **22** | 19-20/20 every run |
| SF12/125 | 3/2/7 | 12 | rarely fires (0-1/20) |

Only two combos are reproducible across all three independent
environments: **SF8/BW125** (near-zero quiet false positives, reliable
real-pulse detection — the strongest evidence Pass B has) and
**SF11/BW500** (consistently the noisiest on quiet, but also reliably
catches the real pulse). The other eight vary enough between runs
(e.g. SF11/BW125: 1→8→4, SF12/BW125: 3→2→7) that 20 samples per
condition isn't enough to pin down their true rates confidently yet.

## Confidence weighting (2026-08-29): `PassBConfidence`, an `energy.csv` column

Shipped as a pure annotation, not a gate: it changes what a `CAD_DETECTED`
row in `energy.csv` *says about itself* to a human reading it, never what
gets logged or promoted. Nothing about whether a `Detection` row gets
created changed — that decision already required an actual decodable
packet during the bounded receive-on-hit window (`radio_task.cpp`'s
`passBCadOneCombo()`), which is a much harder bar than a bare CAD hit and
one noise essentially cannot clear on its own.

`PassBConfidence` (`pass_b_plan.h`) is a 3-value enum — `UNVERIFIED`
(default), `NOISY`, `STRONG` — populated by `passBConfidenceFor(sf,
bw_khz_x10)`, a pure lookup against the two combos with reproducible
cross-run evidence from the pooled 1,200-cycle matrix above:
SF8/BW125 → `STRONG` (2/60 quiet FP, 20/20 pulse every run), SF11/BW500 →
`NOISY` (22/60 quiet FP, 19-20/20 pulse every run). Everything else,
including Pass A's own sf=0/bw=0 rows (no CAD attempted), stays
`UNVERIFIED` — not "safe," just "not yet reproduced."

Deliberately computed at CSV-format time from fields `EnergyObservation`
already stores (`sf`, `bw_khz_x10`), not a new struct field — keeps the
struct at its existing size (still `<=32B`, DESIGN.md §5.2's budget) and
needs no `radio_task.cpp` change at all. `energy.csv` gained one trailing
column, `pass_b_confidence` (`"high"`/`"noisy"`/`"unverified"` — appended
after `radio_status`, same "append not insert" convention `result` itself
already used so existing files stay readable). Native suite 147/147 (two
`test_pass_b_plan` cases for the lookup/name mapping, two
`test_energy_observation` cases for the CSV row); both `cardputer-adv` and
`cardputer-adv-bench` build clean. `HIGH` was tried first as the enum name
and collided with Arduino's own `#define HIGH 0x1` macro (same class of
bug as `LOW`/any other digitalWrite constant) — renamed to `STRONG` before
it ever reached hardware.

## Open questions needing your call

Resolved: off_grid field (yes), K=8 bound (yes), proceed with code (yes),
immediate-vs-deferred timing (immediate, yes), pull and inspect
`energy.csv` (done repeatedly), re-run with randomized/interleaved order
(done), build + test a shielded quiet control (done, two configurations —
shielding raised the quiet baseline rather than lowering it), and ship
per-combo confidence weighting (done — `PassBConfidence`, descriptive
`energy.csv` column only). Remaining:

1. A bigger enclosure or RF-absorptive (not just reflective) lining would
   be needed to actually get a lower-noise control than the open room —
   the foil box's own reflections of the device's self-generated RF now
   look like the dominant effect, not ambient room RF. Given three runs
   have already gone into this and none produced a cleaner baseline than
   simply testing in the open room, this may not be worth further
   equipment investment unless the elapsed-time drift question specifically
   still matters to you.
2. The other eight combos still have only n=20/condition and haven't
   earned a confidence tag either way — more bench cycles per combo (or a
   larger n) would be needed before `NOISY`/`STRONG` could responsibly
   extend past the two that have it now.

## RTL-SDR ground truth for SF11/BW500's false positives (2026-09-02)

Everything above inferred the SF-driven false-positive story from
aggregate quiet-vs-pulse counts — real, but never a direct observation of
what was actually on the air during a specific false `CAD_DETECTED`.
`bench/rtl-sdr/sync_cad_capture.py` closes that gap: it triggers
`BENCH_PASS_B_CAD` for one combo and starts a synchronized RTL-SDR capture
at the same 918.5MHz test point, reading the raw result back via a new
`BENCH_PASS_B_CAD_RESULT` opcode (`bench_fault.h`/`.cpp`) instead of
pulling `energy.csv` off the SD card.

Five quiet-condition attempts at combo 8 (SF11/BW500, `NOISY`): 4/5 came
back `CAD_DETECTED`. The SDR's waterfall for every one of those four was
completely flat at 918.5MHz — no signal, same ambient floor as the quiet
baseline attempt. Direct, not statistical, confirmation that this combo's
false positives really are the radio triggering on nothing, matching the
symbol-duration hypothesis above. See `bench/rtl-sdr/README.md`'s own
"Pass B CAD ground truth" section for the run command and full result.
Not yet run against the other eight combos or with a real pulse as a
positive control — natural next step for question 2 above.
