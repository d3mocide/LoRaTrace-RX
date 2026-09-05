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
- [~] Choose the qualifying RSSI condition and coverage thresholds from the
  matrix rather than from a desk estimate. §6.4 supplies a *candidate* RSSI
  condition (`p90 >= -90 dBm`) and, more importantly, shows why one fixed
  number is not sufficient on its own: the condition's meaning depends on
  sample spacing, and no single threshold separates all 900 trials. The
  coverage thresholds are untouched by this matrix and stay open.
- [ ] Approve the maximum radio-away budget after the Watch-opportunity result.
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
- [~] The controlled matrix is complete with durable SD evidence and a
  location-redacted summary
  ([2026-09-04-phase12-focus-matrix.md](../hardware-results/2026-09-04-phase12-focus-matrix.md)).
  The Watch-opportunity comparison has tooling
  (`scripts/phase12_watch_opportunity.py`) but has not run, so the maximum
  radio-away budget stays unapproved.
- [ ] WiFi-off/on resource matrix, Portland field validation, `STATUS.md`,
  `LOG_GUIDE.md`, release notes, and any companion-schema update reconcile.

Implementation proceeds in two bounded parts. The Engineering foundation now
has pure plan/statistics/CSV tests. The bench request is one pass, 2--2,000 ms
and 2--64 RSSI samples; the matrix gets 30 trials from 30 durable requests,
not one long radio-away loop. Its next slice adds the radio request/recovery
path, logger/health integration, and the bench harness with no coverage label.
Only after §6 selects the thresholds and Watch budget may the operator menu,
Activity surface, durable final schema, and field validation enter the
Device/claim gates.

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
