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
"Hardware verification." Still not resolved by a full bench matrix with a
quiet control.

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

## Open questions needing your call

Resolved: off_grid field (yes), K=8 bound (yes), proceed with code (yes),
immediate-vs-deferred timing (immediate, yes), pull and inspect
`energy.csv` (done — see the SF-vs-detection-rate finding above, which
also resolves what was open question #2 here: the sync-word re-run is no
longer the useful next experiment, the SF/symNum relationship is).
Remaining:

1. The full bench matrix (§ "standing hardware unknown" above) is still
   open, now with a specific hypothesis to test rather than an open-ended
   one: does `CAD_DETECTED` rate at a genuinely quiet bin still scale with
   SF the way it did at these RSSI-elevated peaks? A shielded/attenuated
   quiet control (same standing limitation Phase 8's CAD-rate bench and
   Phase 9's margin bench both already had) would separate "SF-driven
   false positive" from "this specific peak bin happened to be noisy."
2. Worth a per-SF confidence weighting (or a scaled symNum per combo, so
   every entry in `PASS_B_SF_BW_CANDIDATES` gets a comparable false-positive
   exposure instead of SF12 getting ~60x SF7's dwell time) before Pass B's
   `CAD_DETECTED` counts are used for anything beyond "the engine works"?
