# LoRaTrace RX — Version 2 Design

**Status:** draft for product decisions and phased validation.  
**Baseline:** v1.0.7, released 2026-09-04.
**Purpose:** describe the post-v1 direction without rewriting the hardware-verified v1 design, roadmap, or status record.

## 1. Product thesis

Version 1 established LoRaTrace RX as a passive, field-usable instrument:
Watch receives known-channel traffic, Probe tests bounded sourced candidates,
Sweep measures frequency-binned energy, and Field Analyzer presents truthful
views of the data. Version 2 should make those capabilities work together as a
repeatable field-survey workflow:

> **Find an RF condition, investigate it with evidence, preserve the survey
> record, and explain it later.**

The aim is not to add radio modes for their own sake. Each V2 capability must
either improve evidence quality, protect Watch's primary receive role, or make
the existing SD data more useful after a run.

## 2. Confirmed starting point

V1 is the baseline, not a prototype to be rewritten:

- Cardputer-Adv + Cap LoRa-1262 / SX1262, with no PSRAM.
- Usable front-end range is 868-923MHz; 923-928MHz remains outside the tuned
  front end.
- One radio-owner task on Core 1; SD, display, GPS, and WiFi remain outside
  the radio's real-time path.
- Static/bounded storage, fixed queues, streaming metrics, and SD as the
  datastore remain architectural requirements.
- Watch, Probe, Sweep, Cell, and Scope all restore the resolved home
  configuration when their bounded work completes, fails, or is cancelled.
- WiFi remains opt-in and off by default.

V1 hardware evidence also establishes two product facts V2 must respect:

1. A normal Sweep's short per-bin dwell can miss genuine, active traffic. A
   measured empty bin is not proof that the frequency is quiet.
2. Repeated Sweep substantially reduces Watch/Trace's opportunity to receive
   packets while it owns the radio. That is an inherent single-radio tradeoff,
   not a defect that the UI should hide.

## 3. Permanent boundaries

V2 preserves the project's safety, privacy, and truthfulness boundary:

- Receive-only. No transmit, beacon, injection, or active probing.
- Metadata-first. No payload display, retention, decryption, keys, chat, or
  protocol-client functionality.
- No automatic mission-profile selection or protocol claims from energy alone.
- Energy/CAD observations may identify an **unknown LoRa candidate** only when
  the existing evidence rules permit it; they must not be relabeled as
  Meshtastic, MeshCore, Reticulum, or LoRaWAN by inference.
- No fake waterfall, synthetic scope, or continuous background retuning that
  is presented as uninterrupted coverage.
- No feature may silently add unbounded RAM, SD backlog, task lifetime, or
  radio-away time.

## 4. V2 goals

1. **Protect the primary job:** make Watch availability visible and keep it
   the default field behavior.
2. **Make survey evidence cumulative:** distinguish observation coverage from
   radio silence and reveal how many opportunities were actually sampled.
3. **Support deliberate investigation:** let an operator return to an
   interesting frequency or small range with a bounded, longer-dwell survey.
4. **Make runs explainable after the fact:** preserve a compact timeline of
   mode, radio availability, RF observations, GPS quality, and device health.
5. **Move heavy analysis off-device:** make SD data easier to inspect and
   compare without consuming Cardputer heap.
6. **Keep every new claim testable:** source channel plans, define explicit
   budgets, and require real hardware evidence before declaring a phase done.

## 5. Core V2 capabilities

### 5.1 Field Missions — operator-selected radio-time recipes

Field Missions are explicit, named operating recipes. They do not auto-detect
protocols or change profile selection. Their job is to give the operator a
clear answer to: *what is the radio optimizing for right now?*

| Mission | Intent | Radio behavior | Operator promise |
|---|---|---|---|
| **Drive** | Catch known-channel traffic while moving. | Watch-first; no automatic long sweep loops. | Prioritizes receive opportunity. |
| **Stationary** | Characterize a location deliberately. | Operator-approved bounded Survey actions; Watch resumes between them. | Shows time spent observing versus listening. |
| **Investigate** | Revisit a selected Sweep/Waterfall condition. | Runs a bounded Focus Survey over a small selected set, then restores Watch. | Builds evidence for a specific question, not blanket coverage. |

The on-device UI must always show the active mission and a plain-language
availability state such as **WATCHING**, **SURVEYING**, or **RESTORING**. Any
mission that changes runtime behavior requires a menu control as well as an
obvious result/status surface.

**Not in scope:** automatic roaming schedules that retune invisibly, automatic
profile detection, or a promise that Drive has complete-band coverage.

### 5.2 Focus Survey — longer observation of selected RF conditions

Focus Survey addresses the measured dwell-window limitation without turning
the device into a continuously retuned scanner.

An operator selects a frequency or small fixed set of candidate bins from a
Sweep result, Waterfall, or a menu-backed preset. The radio task owns a
bounded request that:

1. snapshots the resolved home configuration;
2. samples only the selected frequencies for an explicit dwell/count budget;
3. computes streaming statistics; and
4. restores home listening on every completion, cancel, timeout, and failure
   path.

The result must report observation evidence, not a binary verdict:

- frequency/band and source of selection;
- observation count and total observation time;
- median, high percentile, and maximum RSSI;
- hit/peak count under a documented condition;
- time spent away from Watch; and
- a coverage state such as **insufficient**, **sampled**, or **repeated**.

Initial implementation uses fixed-size request and result structures. Candidate
frequency count, dwell budget, and output row count are explicit configuration
constants, measured under worst case before shipping. No raw RF samples, packet
bytes, or dynamically growing result history are retained.

### 5.3 Coverage Confidence — truthfulness above the charts

Sweep, Focus Survey, and Cell results should describe the strength of the
*observation* separately from the signal itself.

| Term | Meaning | Must not mean |
|---|---|---|
| **Coverage** | How often/how long the device observed the relevant bin or range. | A guarantee that all traffic was captured. |
| **Observed activity** | A measured RSSI rise, CAD result, or decoded packet under its documented rule. | A protocol identity unless separately proven. |
| **No observation** | The dwell windows did not catch a qualifying condition. | “The frequency is quiet.” |
| **Confidence** | Evidence strength based on repeat observations and calibrated conditions. | Certainty or a protocol verdict. |

V2 should favor an **N of M passes** presentation and a bounded coverage meter
over a single dramatic peak indicator. This matters most in Waterfall and in
any post-run mapping/report surface.

### 5.4 Durable Field Record — compact, explainable run history

Existing detection, session, probe, energy, and cell CSVs remain authoritative
for their current meanings. V2 adds only append-only, purpose-specific records
where the existing schema cannot carry the new fact cleanly.

| Record | Purpose | Suggested content |
|---|---|---|
| **focus.csv** | One row per Focus Survey result/bin. | time, GPS snapshot, selected frequency, coverage, RSSI summaries, source, outcome |
| **mission.csv** | State transitions and radio-time accounting. | mission, action, start/end, requested/completed status, home-restore result, radio-away time |
| **marker.csv** | Operator-created field annotations. | time, GPS snapshot, small preset marker type, optional numeric sequence |

All records are compact fixed-field CSV rows. High-rate raw samples, packet
contents, and raw GPS serial data remain out of scope. Existing CSV schemas
must stay backward-compatible; any column addition follows the project's
append-only schema convention and is documented in LOG_GUIDE.md.

### 5.5 Field Markers — preserve human context without typing on the road

A marker is an explicit operator action that records a location/time context
for later review: for example **start**, **stop**, **parked**, **antenna
changed**, or **site note**. The first version uses a short fixed preset list
and a numeric sequence rather than free-text entry. This keeps the keyboard
flow safe, bounded, and practical in the field.

Markers are not RF detections and must never alter radio behavior. Their value
is correlation: a later report can show that a recurring energy condition
appeared near a marked location or after a hardware change.

### 5.6 Companion Analysis — put rich reporting where the resources are

The Cardputer should acquire trustworthy data; a host-side companion should do
the heavier work. The companion is an offline, reproducible analysis tool that
consumes a copied run folder and produces a report without changing the
original evidence.

Initial deliverables:

- run summary with firmware build, configuration/region, time span, GPS
  quality, health counters, and radio-away accounting;
- frequency occupancy and Focus Survey comparisons across one or more runs;
- route/coordinate plots and time-aligned event views;
- export to a documented shareable bundle; and
- warnings when a conclusion would overstate the run's coverage.

Mapping should work without requiring an online map provider. If location data
is shared, export must offer privacy-aware choices such as rounded coordinates
and pseudonymized node identifiers. The original unmodified SD files remain
the local evidence source.

### 5.7 Region Packs — a research-led expansion, not a table switch

The existing Region setting narrows Sweep to US or Global ranges; it does not
make profile tables international. V2 may add operator-selectable region packs
for known-profile presets, but only after each entry has a sourced frequency
plan, RF parameters, regulatory/range rationale, and hardware validation.

Each pack is a fixed compiled table (or a bounded validated SD configuration),
not a free-form radio configuration UI. Unsupported regions stay explicit;
they are never represented by a guessed “global default.” FCC-specific Cell
labels must not appear as universal claims outside the US context.

### 5.8 Research and validation tooling

V2 should retain the project's evidence discipline. Useful supporting work
includes a repeatable Focus Survey bench harness with known timing and
attenuation conditions, RTL-SDR correlation tools, cross-run regression checks
for CSV schemas/coverage calculations, and a compact field-test template.

This supports product truthfulness; it is not a promise to integrate an SDR,
transmitter, or packet decoder into the Cardputer.

## 6. Proposed build order

The following is a priority order, not a committed release calendar. A
workstream lands only after its scope, static-memory cost, host tests, and
applicable hardware evidence are complete. The canonical gate and version
policy live in `docs/ROADMAP.md`.

| Workstream | Outcome | Why it comes here |
|---|---|---|
| **12. Survey truth** | Coverage vocabulary, persistent per-survey evidence, and bounded Focus Survey. | Directly fixes the most important interpretation gap revealed by V1 testing. |
| **13. Field Missions** | Drive, Stationary, and Investigate recipes with visible Watch availability and radio-time accounting. | Turns individual tools into a coherent operator workflow. |
| **14. Companion analysis** | Offline run report, comparison, and export bundle. | Delivers more value from existing and V2 logs without consuming device heap. |
| **15. Field markers and privacy-aware sharing** | Preset annotations and shareable, redacted report options. | Makes collected data interpretable and responsibly reusable. |
| **16. Cell closeout** | Validate the existing Cell feature's real tower-adjacent RSSI rise and fresh `cell.csv`/`session.csv` output. | A useful bonus feature, deliberately deferred so it does not block the core V2 field-survey workflow. |

The following stay as later candidates until a concrete field question justifies
them: rigorously sourced region packs, richer passive node-activity dossiers,
antenna/install comparative surveys, battery/endurance guidance, and additional
lab correlation tooling. Region packs require their own source-quality,
regulatory/range, and real-hardware-access entry gate; they are not a V2.0
release dependency.

## 7. Architecture and resource rules

### 7.1 Radio ownership and action arbitration

Every V2 acquisition action is a bounded request owned by radio_task. At most
one of Probe, Sweep, Cell, Scope, or Focus Survey may own the radio at a time.
New requests are rejected or visibly queued according to a small fixed policy;
they never block a task waiting for SD, display, WiFi, or another radio action.

Every action must expose:

- request accepted/rejected/cancelled/completed state;
- timeout and a worst-case radio-away budget;
- resolved-home snapshot and restore result; and
- a single operator-visible status/result surface.

### 7.2 Memory and I/O budgets

Before implementation, each workstream gets a one-page budget containing:

- fixed static bytes and stack-frame implications;
- maximum queue depth and row rate;
- expected SD write volume and flush behavior;
- WiFi-on and WiFi-off behavior; and
- heap/stack acceptance measurements from session.csv and serial checkpoints.

New metrics stream into fixed accumulators and then to SD. Whole-spectrum
histories, raw sample arrays, and per-event heap allocation are prohibited.
Static memory is counted as total SRAM, not treated as free merely because it
does not reduce the heap counter.

### 7.3 UI rules

- Results must state source and coverage, not only the largest RSSI value.
- **WATCHING** versus **SURVEYING** is visible at all times.
- New runtime behavior is reachable through the on-device menu, with clear
  start/cancel/recovery feedback.
- Charts remain truthful: Waterfall derives only from measured frequency bins;
  Scope derives only from its bounded single-frequency acquisition.
- The existing indexed canvas is retained unless measurements demonstrate it is
  the limiting allocation; a replacement repeats the full UI/glass matrix.

## 8. Validation gates

No V2 feature is “done” because it compiles or looks good in a mockup. Each
accepted workstream must provide, at minimum:

1. **Host tests** for pure plans, CSV formatting, state transitions, bounds,
   and coverage math.
2. **Hardware state tests** for accepted/rejected/cancelled requests, home
   restore, mutual exclusion, UI entry/exit, and SD output.
3. **Truthfulness tests** that compare on-device claims with known RF timing
   or RTL-SDR ground truth where a claim depends on RF observation.
4. **Resource tests** with WiFi off and on, real GPS/SD workload, task-stack
   watermarks, heap/largest-block trend, and all drop/error counters.
5. **Field evidence** captured under a known build revision and summarized in
   docs/hardware-results with raw location-sensitive artifacts kept private.

Focus Survey and Field Missions additionally require a before/after measure of
Watch packet opportunity. The UI may describe the cost, but it must not imply
that retuned time is free.

## 9. Open product decisions

These questions should be answered before implementation, one workstream at a
time:

1. What maximum radio-away budget is acceptable for Drive, Stationary, and
   Investigate?
2. Should Focus Survey choose individual bins only, a small contiguous window,
   or both?
3. Which coverage thresholds are meaningful enough to label **sampled** or
   **repeated** without pretending to prove absence?
4. Which marker presets are useful in a real wardrive, and what keyboard flow
   makes them safe to create quickly?
5. What is the smallest companion format that is useful without becoming a
   hosted service or requiring cloud access?
6. Which non-US region pack has the best source quality and real hardware
   access for a first validated expansion?

## 10. Definition of V2 success

V2 succeeds when an operator can run a Watch-first drive, deliberately switch
to an investigation at a promising location, return to Watch with a known
cost, and later inspect a compact report that separates measured activity,
observation coverage, device health, and field context.

It does **not** need to become a transmitter, decoder, protocol client, cloud
platform, or unlimited-band analyzer to achieve that.
