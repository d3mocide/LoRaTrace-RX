# Phase 12 Survey Truth — design entry and acceptance plan

**Status:** design entry active; it permits a bench-only raw-counter prototype,
not operator-facing coverage labels or release scope.
**Baseline:** LoRaTrace RX `v1.0.7` on Cardputer-Adv + Cap LoRa-1262.
**Field-validation area:** Portland metro, Oregon. This is a coarse test-area
label only; no coordinates, route, node identity, or claim about local RF
occupancy belongs in tracked evidence.

## 1. Why this phase exists

Phase 9 established that a normal Sweep's short bin dwell can miss genuine
traffic. The real-traffic dwell comparison was inconclusive, and the later
capture investigation showed that packet capture is governed by time parked on
the home channel, not by making a full-band bin dwell slightly longer. Neither
result licenses the UI to call an empty bin quiet.

Phase 12 therefore adds a deliberate, bounded answer to a narrower question:

> At this selected frequency, what did the receiver observe for how long, and
> what did that observation contain?

It does not try to infer a protocol, prove absence, turn Sweep into background
retuning, or improve packet capture by accident.

## 2. Scope and non-goals

The first Focus Survey slice is deliberately small:

- **One selected frequency per request.** The source is one completed Sweep
  or Waterfall bin, or one fixed menu preset. Free-form frequency entry,
  contiguous windows, and multi-bin requests are deferred until this slice's
  actual static-memory and radio-away measurements support them.
- **One bounded radio-owned request.** It snapshots the resolved home channel,
  gathers fixed streaming statistics, writes one durable result row, and
  restores Watch on complete, cancel, timeout, and failure.
- **No raw RF samples or packet bytes.** The radio task retains only fixed
  accumulators; logger/GPS/SD remain outside its real-time path.
- **No identity claim.** RSSI and CAD/packet events, if a later slice adds
  them, remain observations or unknown LoRa candidates under the existing
  evidence rules—not Meshtastic, MeshCore, Reticulum, LoRaWAN, or cellular
  identity by inference.
- **No automatic action.** Focus is operator-selected and menu reachable;
  Drive/Stationary/Investigate recipes belong to Workstream 13.

## 3. Observation and coverage contract

The result presents *coverage* separately from *activity*.

| Term | Definition for Phase 12 | Must not mean |
|---|---|---|
| Requested pass | One requested dwell at the selected frequency with a declared time and sample budget. | A successful measurement. |
| Valid pass | A requested pass that configured the selected frequency, produced its required samples, and was neither cancelled nor radio-error terminated. | A quiet channel. |
| Observation time | Measured elapsed time from the first accepted sample through the last accepted sample, summed only across valid passes. | Time continuously covering a frequency outside those windows. |
| Observed activity | A documented qualifying RSSI condition or independently recorded packet/CAD event during a valid pass. | Protocol identity or continuous activity. |
| No observation | No qualifying event occurred during the valid windows. | “Quiet,” “empty,” or “absent.” |
| Insufficient / sampled / repeated | Coverage labels derived only from valid-pass count and accumulated observation time. | Signal strength, likelihood of absence, or confidence in identity. |

The firmware must store the underlying counts and durations even after a label
is chosen. The display may say `N of M passes` and elapsed observation time;
it must never replace those values with a single confidence word.

### 3.1 Labels not yet frozen

The first implementation must not hardcode arbitrary thresholds. Before the
Device/claim gate closes, the controlled matrix below selects:

1. `FOCUS_MIN_VALID_PASSES` and `FOCUS_MIN_OBSERVATION_MS` for **sampled**;
2. `FOCUS_REPEATED_VALID_PASSES` and `FOCUS_REPEATED_OBSERVATION_MS` for
   **repeated**; and
3. one documented qualifying RSSI condition for the activity count.

Until those constants are accepted, a bench-only prototype may report only `N
valid passes`, requested/completed status, total observation time, and RSSI
summary. It may not show a coverage label or a “no activity” conclusion.

## 4. Bounded data design

`focus.csv` is append-only and purpose-specific. Existing detection, session,
probe, energy, and Cell records keep their current meanings and schemas.

The planned one-row-per-selected-bin result contains the shared run/GPS
columns, then:

```text
rx_uptime_ms,profile,focus_id,selection_source,selection_bin_index,
freq_mhz,requested_passes,valid_passes,requested_dwell_ms,
observation_ms,requested_samples,sample_count,rssi_median_dbm,
rssi_p90_dbm,rssi_peak_dbm,qualifying_count,coverage,request_status,
home_restore,wifi_on,radio_status
```

`coverage` is empty until §3.1 closes. `request_status` distinguishes complete,
cancelled, timeout, and failed; a cancelled or failed request cannot borrow a
coverage label from its partial samples. `home_restore` is recorded separately
because “complete” without Watch recovery is not a valid acquisition result.

### 4.1 Fixed statistics budget

The initial implementation uses one selected bin and a fixed 1 dB RSSI
histogram over a documented bounded receiver range, plus count, sum, maximum,
elapsed-time, and saturation flags. This gives median and P90 without raw
sample retention or a heap allocation. The exact histogram range, bucket type,
request/result struct sizes, queue depth, and worst-case CSV row length are
part of the Engineering-gate budget; the target is a single result state under
256 bytes of static SRAM before queue/control overhead.

This is a target, not a claim of current usage. A wider focus set, dynamic
container, second result history, or per-event allocation is out of scope for
this slice and requires a new budget decision.

### 4.2 Measured budget (2026-09-04)

Counted from the built production image and a host measurement of the
formatter, not estimated. Struct sizes are additionally held by `static_assert`
so a schema change cannot quietly exceed them.

| Item | Measured | Bound |
|---|---|---|
| `FocusRequest` | 12 B | 16 B |
| `FocusObservation` (queue/result record) | 40 B | 48 B |
| `FocusRssiHistogram` (141 one-byte buckets, -140..0 dBm) | 148 B | 160 B |
| One result's working state (histogram + record) | 188 B | 224 B, under §4.1's 256 B target |
| All Focus statics in the production image | 159 B | — |
| Request queue item storage | 1 x 12 B | — |
| Result queue item storage | 4 x 40 B = 160 B | — |
| `radioTask` stack frame | 1,072 B | 6,144 B task stack |
| Worst-case `focus.csv` row | 189 B | 256 B buffer (`FOCUS_CSV_ROW_MAX`) |
| SD write rate | one row per terminal request | — |

Notes on what those numbers do and do not cover:

- The stack frame is the whole radio task's, measured with `-fstack-usage`;
  every bounded action is inlined into it, so it is a shared ceiling rather
  than Focus's own cost. Focus's objects account for at most 216 B of it.
  The device's own measured high-water is `radio_stack_free` in `session.csv`,
  which this budget has not yet read back off a card.
- The worst-case row uses every field at its widest, including widths a
  bounded request cannot produce (a 65,535 ms dwell, a 255-pass count). An
  over-long row is dropped rather than truncated—`focusObservationFormatCsv()`
  returns 0 and the logger counts a dropped row—so the 66 B of margin is
  deliberate and regression-tested at the maximum.
- The 326 B header is written straight to the file (`File::println`) with no
  intermediate buffer, so it is not bound by the row budget.
- Measured radio-away time across the 2026-09-04 behavior run was 74 ms
  (an injected failure, which never sampled) to 1,573 ms (a 1,500 ms injected
  stall). That is fixture timing, not the §6.3 Watch-opportunity decision.

## 5. Radio and UI contract

`radio_task` owns Focus exactly as it owns Probe, Sweep, Cell, and Scope. At
most one bounded acquisition action owns the SX1262. A Focus request is visibly
refused when another action owns the radio; it never waits behind it. Every
exit path restores the resolved home configuration before publishing its final
state.

The on-device menu supplies the runtime control required by `CLAUDE.md`. The
status surface and Activity page show the selected frequency, source, valid/
requested passes, observation time, request state, and recovery result.
`WATCHING`, `SURVEYING`, and `RESTORING` remain mutually exclusive visible
states. Browser observation remains read-only; adding browser acquisition
control would require a separate security decision.

Focus's radio-away measurement starts when it leaves resolved home listening
and ends only after `restoreHomeListen()` reports success or failure. It is a
new Focus-specific health/status value; Sweep's `EA` is evidence for Sweep,
not a substitute for this measurement.

The sample budget alone does not bound that measurement. Every sample waits on
the shared SPI bus, so a contended bus stretches a pass past its dwell while
the radio is away from home. A request therefore also carries a wall-clock
deadline (`focusRequestTimeoutMs()`: dwell plus a slack constant): past it the
request stops sampling and terminates as `timeout`, restoring home like any
other exit path. A recovered timeout stays a timeout—it does not borrow
`complete` from a restore that worked—and it contributes no valid pass and no
observation time. The slack is a bench-slice bound; §6 may revise it.

## 6. Controlled measurement matrix

The matrix establishes whether the product's bookkeeping and wording are true.
It does not seek a universal RF sensitivity number.

### 6.1 Fixture and ground truth

- Use the existing repository-owned Heltec V4 R8 controlled transmitter and
  bench protocol; it is the timing source, not the Cardputer.
- Exercise three sourced US Sweep positions already used by the Phase 9
  low/mid/high harness: 905.3125, 912.8125, and 920.625 MHz. This checks
  selection and bin translation across the accessible US range without
  inventing a Portland channel plan.
- **None of those three sits on a Focus bin center, and the offset is part of
  the measurement.** Focus tunes at the resolved home channel's bandwidth
  (125 kHz on this bench), so where the transmitter sits inside the bin
  changes what a pass can observe. Measured against the US 250 kHz grid:
  905.3125 and 912.8125 MHz are +62.5 kHz from bins 13 and 43, at the edge of
  a 125 kHz passband's half-width; 920.625 MHz sits exactly on a bin boundary,
  125 kHz from either bin 74 or 75 — outside it. A weak or absent rise at the
  high position would therefore measure the offset, not the receiver, so the
  matrix also runs two exactly bin-centered controls from the same repo-owned
  candidate table: `LONG_TURBO` 908.750 MHz (bin 27) and `MESH_OREGON`
  918.500 MHz (bin 66). Every trial records its own offset; an arm that fails
  to separate is reported as unresolved, never as a quiet frequency.
- Record transmitter send timestamps and a time-aligned RTL-SDR capture when
  making an activity-detection claim. The receiver's own `focus.csv` cannot
  prove a missed event.
- Run source-on and source-off controls in alternating order. A source-off
  antenna-in-room run is ambient observation only—not a calibrated false-hit
  rate—unless the fixture/control path itself is known quiet.
- **A source-on trial means the transmitter was radiating for the whole
  window.** One armed pulse at a fixed delay cannot be scheduled against every
  arm: measured airtimes across this candidate table span ~10 ms (SF8/BW250)
  to ~275 ms (SF12/BW125), and the receiver's window opens tens of ms after
  the request is accepted, so a delay that lands inside a 2,000 ms dwell
  misses a 100 ms one entirely. The first matrix attempt measured exactly
  that: at 100 ms, source-on and source-off were indistinguishable because the
  pulse began after the window closed. The runner therefore re-arms
  continuously for the trial's duration, paced by the transmitter's own
  `TX_DONE` — a fixed interval shorter than the airtime queues overlapping
  sends whose tail bleeds into the next trial, which the second attempt then
  measured as source-off trials reading -26 dBm.
  This makes the matrix a measurement of the qualifying RSSI condition under a
  known-present source. It is explicitly **not** a catch-probability estimate
  for intermittent traffic; §1 already records that comparison as inconclusive
  and this is not a second attempt at it.
- The bench source is also far stronger than realistic traffic (source-on
  reads near -26 dBm against a -100 dBm floor at these antenna separations).
  A threshold selected here separates a strong known source from ambient; it
  is not a sensitivity limit and must not be presented as one.

The SX1262 documentation and its CAD application note remain the authoritative
hardware references; device-specific timing and RSSI behavior are accepted
only from this board's measured matrix, not copied from a datasheet.

### 6.2 Candidate dwell matrix

Test three candidate dwell budgets—100 ms, 500 ms, and 2 s—at each fixture
frequency. They are measurement arms, not shipped defaults. For each arm:

1. run at least 30 source-on and 30 source-off requests in randomized or
   alternating order;
2. retain transmitter log, Cardputer framed status/boot identity, fresh
   `focus.csv`, `session.csv`, and SDR correlation where applicable;
3. verify count/time arithmetic against timestamps and the independent source;
4. record request completion, cancellation latency, timeout, failure, restore,
   queue/row drops, and actual radio-away time; and
5. report activity opportunity as a binomial proportion with a 95% interval,
   never as a certainty. Use Wilson or exact intervals rather than a symmetric
   normal approximation at small counts.

`scripts/phase12_focus_matrix.py` runs the arms and writes one JSONL row per
trial; it requires explicit `--allow-transmit`, quiets the transmitter on every
exit path, and rejects a trial that loses home restore, misses its sample
count, or drops a queued or durable row. It selects no threshold.
`scripts/phase12_focus_matrix_report.py` is the offline, deterministic analysis
over that file: per arm it prints the source-on and source-off distributions,
the lowest 1 dB condition that separates them (or reports that they overlap),
and Wilson 95% intervals for both detection proportions. Neither script can
emit a coverage label.

An arm is rejected if it loses radio ownership, cannot restore home listening,
has nonzero unexplained queue/row drops, or its durable row disagrees with the
measured request. No hit-rate target is set until the source waveform, link
budget, and qualifying condition are locked; a deceptively precise percentage
would answer a different question than coverage.

### 6.3 Watch-opportunity measurement

Before accepting a dwell/pass budget, compare:

- Watch-only baseline at the active home channel against the independent
  transmitter/reference count; and
- the same interval with repeated, explicitly bounded Focus requests.

Report received/reference packets with a 95% interval, Focus away time,
completed requests, and the distribution of time between restored Watch
windows. The acceptance decision is an operator product decision after the
measurement; it must not imply that an away-time display makes the loss free.

`scripts/phase12_watch_opportunity.py` runs both arms against one independently
timed pulse train and reports exactly those figures. Two constraints make the
comparison mean anything, and the script enforces the first and reports the
second:

- The reference train must be receivable by Watch, so it uses the one
  transmitter candidate matching this bench's resolved home channel
  (`MESH_OREGON`, 918.5 MHz SF8/BW125/CR4-5/sync 0x2B) and refuses to run if
  the device's home channel is something else. Otherwise both arms measure
  nothing and their equality would look like a reassuring result.
- The loss figure is only interpretable next to the **away fraction**: the
  share of the arm's wall time Focus actually held the radio. Host round-trip
  latency inflates the idle gaps between requests, so a small measured loss at
  a small away fraction says nothing about a larger duty cycle. The script
  prints both and refuses to reduce them to one number.

### 6.4 Matrix result (2026-09-04)

900 trials completed with zero transport errors, zero queue or row drops, and
successful home restore on every trial. The full location-redacted summary is
[docs/hardware-results/2026-09-04-phase12-focus-matrix.md](../hardware-results/2026-09-04-phase12-focus-matrix.md);
the three results that change this design are:

1. **Detection tracks the source's airtime against sample spacing, not dwell
   length.** With a fixed 8 samples, spacing is `dwell / 7` — 14/71/286 ms for
   the three arms. Sources with airtime far above the spacing were caught in
   every arm; as airtime approached and fell below it, worst-case source-on
   degraded monotonically and one arm stopped separating entirely. This is §3's
   "observation time is not coverage" warning made concrete: **a 2000 ms dwell
   observes eight instants, not 2000 ms**, and lengthening the dwell at a fixed
   sample count makes a pass *worse* at catching bursts. A later slice should
   scale the sample budget with the dwell; §4's schema and any operator-facing
   dwell/observation display must not imply continuous coverage.
2. **Bin-center offset was not the dominant term.** The +125 kHz `high`
   position still separated by ~50 dB at the two shorter dwells while a
   perfectly centered control separated by less. The §6.1 offset concern is
   not what distinguishes these arms at this source strength.
3. **Radio-away is dwell plus ~73 ms**, worst case 2,139 ms over 900 trials.

And the two the matrix could not settle:

- **No single fixed RSSI condition separates every arm.** Pooled, the sets
  overlap (source-on min -99.0 dBm vs source-off max -75.0 dBm). `p90 >= -90
  dBm` is the nearest candidate — no missed source-on and 1 ambient false hit
  in 420 across the 14 separating arms — but it is a candidate only, measured
  against a source ~70 dB above ambient, and its meaning is tied to the sample
  spacing of the arm it ran at.
- **Coverage thresholds remain unselected.** They are about pass counts and
  accumulated time across repeated requests, which a single-pass matrix cannot
  supply. `coverage` stays blank and the labels stay undisplayable.

## 7. Portland metro field validation

After controlled acceptance, run a small stationary field validation in the
Portland metro, Oregon area. It checks usability and evidence durability, not
local spectrum identity or coverage of the city.

- Select only conditions already visible in a completed Sweep/Waterfall or a
  fixed preset; record their source in `focus.csv`.
- Use a small number of operator-chosen stops; retain exact coordinates and raw
  CSVs privately. Commit only a location-redacted summary with build revision,
  rough conditions, requested/completed passes, radio-away time, health
  counters, and caveats.
- Confirm on-device selection/cancel/recovery feedback, SD removal/error
  behavior, and WiFi-off/on resource behavior under real GPS/SD workload.
- Treat a no-activity result as a bounded observation at that stop. Do not use
  it to map a quiet area or publish a protocol or operator identity claim.

## 8. Gate checklist and implementation order

### Design entry — active

- [x] V2 boundaries, one-bin first slice, result vocabulary, fixed-statistics
  direction, controlled matrix, and Portland field-validation handling are
  recorded here.
- [x] Choose the qualifying RSSI condition from measurement rather than a desk
  estimate — **measured and rejected**. §6.4's `p90 >= -90 dBm` candidate came
  from a source ~70 dB above ambient. Repeated at a realistic level (source
  peak ~-85 dBm against a -99/-100 dBm floor, 120 trials), it fails, and so
  does every floor-relative variant: one of ten metric/position combinations
  separates, by 2 dB, which is inside ordinary RSSI variance. An RSSI summary
  statistic cannot support an activity claim at field levels. See
  [the evidence summary](../hardware-results/2026-09-04-phase12-focus-matrix.md).
  Coverage reporting is unaffected; what is refused is the step from "RSSI was
  elevated" to "something transmitted", which is the step §3 forbids anyway.
  The count route named there has since been **measured and survives**: a
  per-pass count of samples above the pass's own median separates where every
  summary statistic failed. `C6 >= 2` (two or more samples at median + 6 dB)
  detected 57/60 source-on trials, 95% CI [0.863, 0.983], with one flagged
  source-off trial that read -63 dBm against a -101 dBm median — a real
  transmission, not ours, so the 1.7% false rate is an upper bound rather than
  a measurement against true silence. Three further campaigns (1,200 trials) then
  characterised it.

  **Form.** It is a *fraction* of accepted samples, roughly 4-8%, which
  transfers between 500 ms and 2,000 ms passes. No threshold rescues a 100 ms
  pass: six samples cannot both catch the source and reject ambient, and 13 dB
  of link improvement barely moved it (23/60 to 28/60), so that is a sampling
  limit rather than a link limit. **A short pass may report coverage and must
  not report activity.** 500 ms / 26 samples was the strongest arm at both
  measured links.

  **Scope, after a correction.** An earlier reading of one link concluded the
  rule "detects a persistently occupied channel, not individual packets". A
  second baseline withdrew it: at 28.6% occupancy, 13 dB took detection from
  37% to 87%, so the apparent cliff belonged to that link. The positive case
  was then measured directly — one armed 148 ms packet inside a 2,000 ms pass,
  7.4% occupancy, detected **29/30 with 0/30 false positives**. A 148 ms
  packet sampled every 20 ms yields about seven elevated samples against the
  four the rule needs, so nothing about the instrument prevented it.

  **The condition that remains.** Detection is a function of link quality the
  device cannot know: the same rule at the weaker link failed on sources
  occupying four times as much of the pass. And a position carrying real
  traffic measured *worse*, not better — a -57 dBm event in a control trial
  both produced false positives and suppressed counts by lifting the median,
  so Focus is least reliable exactly where a band is busiest. Wording an
  activity indication that stays true under those conditions is a product
  decision now, not an open measurement. CAD or packet reception remains §3's
  alternative, neither favoured nor excluded by this evidence.
- [ ] Coverage thresholds (`sampled`/`repeated`) remain unselected. They are
  about pass counts and accumulated time across repeated requests, which no
  single-pass measurement can supply. `scripts/phase12_coverage_campaign.py`
  drives that campaign. **Their stakes dropped with the activity decision
  below**: with no activity claim resting on them, `sampled`/`repeated` are
  descriptive — how much looking happened — rather than the qualifier on an
  inference.
- [x] **Approve the maximum radio-away budget — refused, conditionally
  (2026-09-06). See "Decisions" below.** **The measurement is complete**
  ([evidence](../hardware-results/2026-09-04-phase12-focus-matrix.md)): at a
  2,000 ms dwell and 48.1% away fraction, Watch reception fell from 0.883 to
  0.463 of a reference train, with non-overlapping 95% intervals. The loss is
  proportional to away time and nothing else — predicted 0.459 against a
  measured 0.463 — so Focus's recorded away duration is an honest proxy for
  what a request costs, and there is no hidden retune or recovery penalty.
  **The budget decision itself remains open**, deliberately: this supplies the
  exchange rate, not the policy, and §6.3 requires the decision to be an
  operator product judgement made after the measurement rather than implied
  by it.
- [x] Count actual static SRAM, stack frame, queue, row-length, and SD-rate
  budgets (§4.2). Counted after the vertical slice rather than before it, on
  the built image instead of on paper; the numbers land inside the bounds this
  section set, so nothing had to be resized.

### Engineering gate

- [x] Pure one-bin request, histogram/percentile, CSV, and restore-before-
  publish state tests pass. The fixed RSSI histogram is 141 one-byte buckets
  (-140 through 0 dBm);
  its 148-byte working state plus the 40-byte result record total 188 bytes,
  below the 256-byte one-result target before queue/control overhead.
- [x] Focus plan/statistics/runtime and framed-control native tests pass, and
  both production and bench firmware build. Focus is now linked into the
  bounded bench image; the production image retains the same source but
  rejects every bench trigger.
- [x] Fixed request/result/queue/CSV budgets are measured and accepted: see
  §4.2. Every struct is inside its `static_assert` bound, one result's working
  state is 188 B against the 256 B target, and the worst-case row is 189 B
  against a 256 B buffer that drops rather than truncates.
- [x] A bounded bench harness extends the existing framed Serial Control
  pattern; no ad-hoc USB text parsing is treated as evidence.
- [x] A bench-only prototype now has a Core-1-owned one-bin request, Core-0
  `focus.csv` queue/writer, and restore-before-publish terminal row. It is
  entered only by the bench-image `BENCH_FOCUS bin:dwell_ms:samples` framed
  command; production rejects that command. `BENCH_FOCUS_RESULT` returns only
  the latest compact, GPS-free fixed-point summary to make the fixture
  reproducible; the durable record remains `focus.csv`.
- [x] Two paired smoke checks on the bench image confirmed quiet/source-on
  requests at US Sweep bin 43 (912.750 MHz), each 500 ms/8 samples. The
  controlled Heltec `LONG_MODERATE` pulse (912.8125 MHz, capped -9 dBm)
  completed inside each source-on window. Quiet P90/peak was -101.0/-101.0
  dBm; source-on P90/peak was -82.0/-82.0 then -87.0/-87.0 dBm, a 14--19 dB
  rise. All rows reported successful home restore and durable `focus.csv`
  writes. These are transport/RSSI smoke checks, **not** a threshold,
  calibration, coverage, or activity-detection claim. The reusable
  `scripts/phase12_focus_bench.py` requires `--with-pulse --allow-transmit`
  before it may arm the transmitter.
- [x] The non-transmitting `scripts/phase12_focus_behavior_bench.py` hardware
  fixture confirmed two cancelled Focus requests, an injected post-retune
  failure, and two-way refusal with Probe and Sweep. Every Focus terminal row
  was durably written only after home restore; the failed row retained the
  injected operation error after recovery.
- [x] The request is bounded in wall-clock time, not only in samples (§5), and
  the timeout path is reachable on demand: `BENCH_FOCUS`'s optional fourth
  field arms a one-shot bench sample-loop stall, since production only times
  out under real bus contention a fixture cannot arrange. Cell and Scope are
  menu-only actions in production, so the bench-only `BENCH_ACTION` opcode
  starts, cancels, and reports them through the same request functions the
  menu calls—without it, Focus's mutual exclusion against them has no
  reproducible fixture. Both are bench-image gated; production rejects them.
- [x] Sizing `STATUS` by hand had left its 240-byte argument buffer past the
  frame's own ~230-byte budget once Phase 12 added six fields, and an
  over-long frame is dropped silently rather than truncated—losing exactly the
  newest fields the fixtures read. The argument budget is now derived from the
  frame size, host-tested at saturation, and the frame limit raised to 384.

### Decisions (2026-09-06)

Two gates were measurement-complete but decision-open. Both are settled here,
with reasoning, because a decision recorded only as a closed checkbox gets
re-litigated the first time someone reads the evidence and reaches a different
conclusion.

The principle both follow, in the operator's own words: **Focus is a deliberate
use, not a runtime state.** It is an act someone performs on purpose, with a
question in mind, and it should be designed as one.

#### 1. Radio-away budget — refused, with an expiry condition

No maximum away budget is imposed. The exchange rate is measured and clean
(loss proportional to away time and nothing else; predicted 0.459 against a
measured 0.463), and §6.3 explicitly permits approving *or refusing*.

Refusing is right for the shape Focus actually has:

- **There is no runaway path.** One Enter is one pass. Sweep and Cell have
  repeat modes; Focus does not, and its request contract is a single pass
  (`FOCUS_BENCH_REQUESTED_PASSES`).
- **The cost is already legible.** Activity's `AWAY T` card shows the longest
  away time and which tool spent it, and the loss is linear in that number, so
  an operator can self-govern against something already on screen.
- **A cap would cost more than it buys.** It needs a refusal path, a UI state,
  and — per CLAUDE.md's own rule that new runtime behavior gets an on-device
  control — a menu toggle, all for a failure mode that cannot presently occur.

**This decision expires the moment Focus gains automatic repetition.** If Focus
ever grows an `R` binding like Sweep's, the self-governing argument evaporates
and the budget must be settled before that ships. The condition is recorded in
`focus_plan.h` next to the code that would implement it.

#### 2. Activity indication — refused. Focus reports coverage, never activity

Focus does not, and will not, tell an operator that something transmitted.

The measurement supports a rule: `C6 >= 2` detected 57/60 source-on trials
(95% CI [0.863, 0.983]) with a false rate whose 1.7% is an upper bound rather
than a measurement against true silence. It is the condition attached to it
that disqualifies it as a product claim:

- Detection is a function of **link quality the device cannot know**, so the
  device cannot bound its own error, which is exactly the situation §3 forbids
  making a claim in.
- It measured **worse where the band was busiest** — a −57 dBm event in a
  control trial both produced false positives and suppressed counts by lifting
  the median. An indicator that degrades precisely where it is most wanted
  fails in the direction of false confidence, which is the worst direction
  available.

Nothing measured is lost. `qualifying_count` and the full count-above-median
ladder are still recorded in `focus.csv` and over `BENCH_FOCUS_COUNTS`, so a
host with context the device lacks — RTL-SDR ground truth, a known link — can
evaluate any rule offline. **The device measures; the analyst concludes.**

What this costs is the headline: Focus is "a longer look at one bin", not "is
something there". That is what it is, and declining to overclaim is the
project's stated differentiator, not a consolation.

### Device, claim, and release gates

- [x] Hardware proves timeout plus mutual exclusion against Cell and Scope.
  `scripts/phase12_focus_behavior_bench.py` drove all four on the bench image
  (`V=1.0.7;R=3e31daa-dirty;BENCH=1`, 2026-09-04): a 1,500 ms injected stall
  against a 100 ms/1,100 ms-deadline request terminated as `timeout`
  (`RS=2`) with successful home restore and 1,573 ms total radio-away time,
  and Focus and Cell, then Focus and Scope, each refused the other while it
  owned RX. Six terminal requests across the run wrote six durable rows with
  zero queue or row drops (`FW` 0->6, `FD`/`FL` 0), which is the logger's
  post-write counter, not an enqueue count. Timing evidence is the receiver's
  own; it is not a coverage, calibration, or activity claim.
- [x] The controlled matrix and the Watch-opportunity comparison are both
  complete, with durable evidence and a location-redacted summary
  ([2026-09-04-phase12-focus-matrix.md](../hardware-results/2026-09-04-phase12-focus-matrix.md)).
  Between them they settled the sampling policy, rejected the qualifying RSSI
  condition, and measured Watch's cost. What they left open is recorded above:
  the away-time budget decision, the coverage thresholds, and an activity
  basis that is not an RSSI summary.
- [ ] WiFi-off/on resource matrix, Portland field validation, `STATUS.md`,
  `LOG_GUIDE.md`, release notes, and any companion-schema update reconcile.

### Next, in order

**Revised 2026-09-06.** Steps 1, 2 and most of 5 have since closed; what is
left is no longer measurement. Struck items are kept rather than deleted so
the ordering argument stays readable.

1. ~~**Scale the sample budget with the dwell**~~ — **done.** `focus_plan.h`
   derives sample count from dwell at a measured 20 ms spacing
   (`FOCUS_SAMPLE_SPACING_MS`, `focusSamplesForDwell()`), chosen because
   detection tracks the source's airtime against `dwell/(samples-1)` rather
   than against dwell. A 94 ms source was missed at 286 ms and 100 ms spacing
   and caught 15/15 at both 50 ms and 20 ms; 20 ms keeps roughly 2x margin
   against the ~40-50 ms airtime of the fastest realistic mesh traffic. Finer
   sampling is free in the only currency that matters: measured away time was
   2,073-2,075 ms across every arm, whether the pass took 8 samples or 101.
2. ~~**Re-select the qualifying RSSI condition**~~ — **measured, and the
   answer is that no RSSI summary supports the claim.** See the Design-entry
   item above: every summary statistic failed at realistic levels, the count
   route (`C6 >= 2`) survived 1,200 trials, and its remaining condition is a
   link quality the device cannot know. **What is left is a product decision,
   not a measurement.**
3. **Approve, or refuse, a maximum radio-away budget.** The exchange rate is
   measured and clean — loss is proportional to away time and nothing else,
   predicted 0.459 against a measured 0.463 — so this too is now a product
   decision. §6.3 requires it be made *after* the measurement rather than
   implied by it, which is where it now sits.
4. **Select the coverage thresholds.** Still genuinely open, and still the
   only remaining item that needs new measurement: these are about valid-pass
   counts and accumulated observation time across *repeated* requests, which
   no single-pass matrix supplies. `scripts/phase12_coverage_campaign.py`
   drives that campaign.
5. Mostly done. The operator control exists (Focus is Activity's view 4;
   Enter surveys the last Sweep peak, or the home channel's bin if no sweep
   has completed), the Activity/status surface shipped with it, and
   `LOG_GUIDE.md` now carries the operator-facing `focus.csv` section that was
   deliberately withheld until a control existed. Remaining: the durable
   schema's `coverage` column stays empty until step 4, and Portland field
   validation is unrun.

The release is now gated on two product decisions and one measurement, not on
engineering. **If the answer to 2 is "Focus reports coverage and never
activity", that closes the gate** — but it has to be written down as a
decision, or it will keep reading as an unfinished measurement.

## Sources

- Project evidence: `docs/STATUS.md` (Phase 9 dwell, capture-window, and
  RTL-SDR findings); `src/radio_task.cpp` / `.h` (bounded ownership and Sweep
  away-time precedent); `scripts/phase9_bin_accuracy_bench.py` and
  `scripts/bench_harness.py` (existing controlled-fixture pattern).
- [Semtech SX1262 resources](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262)
  — datasheet and SX126x CAD application note catalogue; use them for hardware
  API/reference behavior, not as a substitute for board measurements.
- [NIST confidence intervals for proportions](https://www.itl.nist.gov/div898/handbook/prc/section2/prc241.htm)
  — Wilson and exact interval guidance for small-count activity-opportunity
  measurements.
