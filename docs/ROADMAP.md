# LoRaTrace RX — V2 Roadmap

This is the active, forward-looking gate board for LoRaTrace RX. It begins
from stable `v1.0.7` and governs V2 work only. The completed v1 phase
narrative, feasibility analysis, exit criteria, and versioning history remain
at [history/ROADMAP_V1.md](history/ROADMAP_V1.md).

Start with [STATUS.md](STATUS.md) for what is true on hardware now and
[research/V2_DESIGN.md](research/V2_DESIGN.md) for V2 product boundaries.
This document records what may enter implementation next, what proof it needs,
and what earns a release.

## Permanent boundaries

V2 preserves the shipped foundation:

- Receive-only; no transmit, beacon, injection, decryption, keys, payload
  display, or protocol-client behavior.
- One radio-owner task on Core 1; at most one bounded acquisition action owns
  the SX1262. SD, display, GPS, and WiFi never block its real-time path.
- Fixed/static storage, bounded queues, streaming metrics, and SD as the
  datastore. New capability may not silently add unbounded RAM, SD backlog,
  task lifetime, or radio-away time.
- Watch remains the default. Every acquisition completion, cancel, timeout,
  and failure restores the resolved home configuration and records its result.
- RSSI/CAD/packet evidence never becomes a protocol identity merely by
  inference. A missed dwell is not a quiet frequency.
- WiFi remains opt-in and browser acquisition control remains out of scope
  without a separate security decision.

The detailed rationale and product wording live in
[research/V2_DESIGN.md](research/V2_DESIGN.md); do not duplicate it here.

## Status and gate model

| Status | Meaning |
|---|---|
| **Not entered** | Entry decisions are not yet locked. |
| **Design entry** | Scope and the measurement plan are being locked; there is no release claim. |
| **Engineering** | Code and host validation are in progress. |
| **Hardware pending** | The implementation gate is met, but device proof is incomplete. |
| **Closed** | Every applicable gate has accepted evidence. |

Every workstream passes these gates in order:

| Gate | Required proof |
|---|---|
| **Design entry** | Scope, non-goals, operator promise, unresolved decisions, log-schema impact, and worst-case memory/queue/SD/radio-away budget are explicit. |
| **Engineering** | Host tests cover plans, bounds, state transitions, CSV formatting, and coverage math; native tests and the production build pass. |
| **Device behavior** | A real device proves request/refuse/cancel/timeout/failure paths, mutual exclusion, home restore, UI behavior, and fresh SD output. |
| **Claim truth** | Controlled RF timing or RTL-SDR ground truth validates every RF/coverage claim; absence of an observation is never relabeled as silence. |
| **Release** | WiFi-off/on resource evidence, redacted field summary, `STATUS.md` reconciliation, and operator release notes are complete. |

Focus Survey and Field Missions also require a before/after measurement of
Watch packet opportunity. Displaying a radio-away duration does not make the
cost acceptable; the measurement is part of the decision.

## V2 workstreams

| Workstream | Status | Outcome and phase-specific exit gate |
|---|---|---|
| **12 — Survey truth** | **Engineering** | Define coverage vocabulary and persistent per-survey evidence, then add bounded Focus Survey. The active [design-entry and acceptance plan](research/phase12-survey-truth-design.md) locks the one-bin first slice and controlled matrix; radio-away budgets and sampled/repeated thresholds remain measurement-gated. |
| **13 — Field Missions** | Not entered | Add Drive, Stationary, and Investigate as explicit recipes with visible WATCHING/SURVEYING/RESTORING and `mission.csv` accounting. Prove transitions do not hide radio-away time or weaken action arbitration. |
| **14 — Companion analysis** | Not entered | Deliver an offline, reproducible tool that reads copied run folders without changing original evidence. Test deterministic reports, multi-run comparison, coverage warnings, and privacy-safe export behavior. |
| **15 — Field markers and sharing** | Not entered | Add fixed, safe marker presets and `marker.csv`, then integrate redacted sharing. Prove markers cannot affect radio behavior and realistic exports remove selected location/identity detail. |
| **16 — Cell closeout** | Deferred bonus | Close the existing V1 Phase 11 evidence gap: a real tower-adjacent RSSI rise plus fresh SD verification of `cell.csv` and Cell's appended `session.csv` fields. This preserves V1 history; it does not renumber it. |

Rigorously sourced region packs are later candidates, not Workstream 16 and
not V2.0 blockers. Each proposed pack needs a separate entry gate with source
quality, regulatory/range rationale, fixed-table validation, and realistic
hardware access.

## Current work — Workstream 12

The workstream is in Engineering. Its bench-only first slice now exercises one
bounded radio request, fixed statistics, and `focus.csv` persistence; it has
not added a production control or coverage/activity claim. The plan sets the
following constraints:

- One selected Sweep/Waterfall bin or fixed preset per request; no free-form
  frequency entry, arbitrary range, or multi-bin list in the first slice.
- `focus.csv` is an append-only result record, not raw sample history.
- A fixed histogram/statistics accumulator supplies median, P90, and maximum
  without heap allocation; actual static/queue/row budgets must be measured.
- `insufficient`, `sampled`, and `repeated` cannot be displayed until a
  controlled transmitter/RTL-SDR matrix selects their pass/time thresholds.
- Portland metro, Oregon is the privacy-preserving field-validation area;
  exact location and raw GPS-bearing artifacts remain private.

Engineering may build a bench-only raw-counter prototype so the matrix can
measure the open decisions. It may not present a coverage label or a “no
activity” conclusion until the Device/claim gate closes.

## V2.0 composition release

`v2.0.0` is earned only when Workstreams 12–15 pass together on one identified
build. It repeats cross-feature risks rather than aggregating old checklists:

- every bounded radio action mutually excludes correctly;
- complete, cancel, timeout, and failure restore resolved home listening;
- append-only CSVs remain readable by the companion;
- WiFi-off/on resource trends remain healthy; and
- a field workflow demonstrates Watch-first driving, deliberate investigation,
  known radio-away cost, and an explainable offline report.

Workstream 16 (Cell closeout) is intentionally not a V2.0 blocker.

## Version and release policy

- `src/version.h` is the semantic-version source of truth. A release tag must
  match it; CI rejects a mismatch. Each tagged release needs operator-facing
  notes in `docs/RELEASE_NOTES.md`.
- `v1.0.x` is the stable maintenance line. The completed phase-number mapping
  belongs to [the v1 archive](history/ROADMAP_V1.md).
- A closed core V2 workstream may earn the next stable minor release:
  `v1.1.0` through `v1.4.0` for Workstreams 12–15. Workstream numbers remain
  roadmap identities, not version components.
- `v2.0.0` requires the composition release gate above.
- Workstream 16 evidence closeout without an operator-facing behavior change
  updates status only. A later Cell improvement may earn a `v2.1.0`-class
  release after its own gates.
- A rolling `dev-latest` build remains for day-to-day hardware testing; it is
  not a release gate and must be identified by build revision.

## Evidence and history

Use this order when deciding a gate:

1. `docs/STATUS.md` for current accepted facts and open hardware work.
2. The workstream's design-entry document for scope, measurement method, and
   acceptance criteria.
3. `docs/hardware-results/` for location-redacted evidence summaries; keep raw
   serial/CSV/GPS artifacts in its git-ignored `private/` area.
4. `CHANGELOG.md` for terse post-v1 decision history.
5. [history/ROADMAP_V1.md](history/ROADMAP_V1.md) only for v1 Phase 0–11
   questions or original v1 gate rationale.

Historic code comments and changelog entries that mention a V1 Phase refer to
the archive unless they name a later research or hardware-results record.
