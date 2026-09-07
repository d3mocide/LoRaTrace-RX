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

- Receive-only; no transmit, beacon, injection, or protocol-client behavior.
  Raw payload display is allowed (2026-09-05). Public-channel and known
  operator-key decryption are allowed (2026-09-07); brute-force key recovery
  is out of scope. See the amended boundaries below.
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

## Amended boundary: on-device payload display

Until 2026-09-05, "payload display" sat alongside decryption and transmit as a
permanent prohibition. It is now allowed on-device, by operator decision. The
reasoning is recorded because a future reader will otherwise find a boundary
and a feature that contradict each other and be unable to tell which is
current.

**What changed.** The device already records raw received bytes to SD
(`detections.csv`), and the companion tooling reads them. Refusing to show on
the device what is already written to the card, and already readable by
anything that reads the card, protected nothing — it made the hardware less
useful for RF triage without making anyone's traffic less exposed. The
Captures inspector is the concrete case: signal quality and framing detail are
diagnostic, and the payload bytes alongside them are what make a capture
interpretable in the field rather than back at a desk.

**What remains restricted.** No transmit, beaconing, injection, or
protocol-client behaviour. Bytes may be shown as received; decryption follows
the 2026-09-07 policy below. Neither readable bytes nor successful decryption
alone establishes authenticated identity.

**What this obliges.** Displaying payload makes the operator's screen a
disclosure surface: a captured payload belongs to whoever sent it. Sharing and
export features (Workstream 15) must treat displayed payload as at least as
sensitive as location, and the redaction work there covers it rather than
treating it as already-public because it was on screen.

## Amended boundary: passive decryption and known keys

**Operator decision, 2026-09-07.** Public-channel decryption and decryption
using known operator-supplied keys are within project scope. Brute-force,
dictionary attacks, key guessing, and automated key recovery are out of scope.
This permission does not introduce active protocol participation.

The shipping decoder uses Meshtastic's published default public-channel PSK
for NodeInfo only; MeshCore adverts are parsed without signature verification.
General plaintext decoding and operator-key support are not implemented.
Future operator-key use requires explicit configuration and an on-device opt-in,
disabled by default. This policy update does not change firmware defaults.

Before implementing operator-key support, define key import, storage, removal,
visibility, and bounded decoding costs. Keys must never enter source control,
serial diagnostics, capture CSVs, browser responses, or shareable exports.
Preserve raw observations separately from derived plaintext and record decoder
provenance and authentication status without recording keys. Decoded content
is sensitive even when its channel key is public; sharing must redact it along
with location and identity. Validate malformed inputs and capture loss under
maximum decode load. The physical-access limits in `SECURITY.md` still apply.

## Status and gate model

| Status | Meaning |
|---|---|
| **Not entered** | Entry decisions are not yet locked. |
| **Design entry** | Scope and the measurement plan are being locked; there is no release claim. |
| **Engineering** | Code and host validation are in progress. |
| **Hardware pending** | The implementation gate is met, but device proof is incomplete. |
| **Closed** | Applicable gates have accepted evidence or an explicit, documented operator exception; exceptions remain visible and are not completed tests. |

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
| **12 — Survey truth** | **Closed (v1.1.0, 2026-09-07)** | Coverage vocabulary, persistent per-survey evidence, and bounded Focus Survey, shipped with an operator surface. Coverage thresholds selected from measurement (`focus_coverage.h`); the radio-away budget **refused** while Focus stays one-Enter-one-pass, and Focus **reports coverage and never activity** — the rule that survived measurement depends on link quality the device cannot know. **Portland field validation is deferred, not done**, by explicit operator decision; recorded in `docs/STATUS.md`. |
| **13 — Field Missions** | Not entered | Add Drive, Stationary, and Investigate as explicit recipes with visible WATCHING/SURVEYING/RESTORING and `mission.csv` accounting. Prove transitions do not hide radio-away time or weaken action arbitration. |
| **14 — Companion analysis** | Not entered | Deliver an offline, reproducible tool that reads copied run folders without changing original evidence. Test deterministic reports, multi-run comparison, coverage warnings, and privacy-safe export behavior. |
| **15 — Field markers and sharing** | Not entered | Add fixed, safe marker presets and `marker.csv`, then integrate redacted sharing. Prove markers cannot affect radio behavior and realistic exports remove selected location/identity detail. |
| **16 — Cell closeout** | Deferred bonus | Close the existing V1 Phase 11 evidence gap: a real tower-adjacent RSSI rise plus fresh SD verification of `cell.csv` and Cell's appended `session.csv` fields. This preserves V1 history; it does not renumber it. |
| **17 — Sweep/Waterfall sampling review** | **Design entry** | Re-evaluate whether Sweep's per-bin sampling and Waterfall's presentation can support what they imply, using the measurement apparatus Workstream 12 built. Entry needs a two-baseline sensitivity measurement, not an argument from analogy. See below. |

## Shipped — v1.1.0 operator UI slice

Focus, the Activity dashboard, and the Captures packet inspector shipped in
v1.1.0. Coverage labels now use `focus_coverage.h`; they describe accumulated
observation effort and never activity. The radio-away policy remains the
explicit one-Enter-one-pass exception, which expires before automatic or
multi-bin scheduling is introduced. Palette switching was outside this slice.

## Candidate — Workstream 17 (Sweep/Waterfall sampling review)

Raised 2026-09-05 out of Workstream 12's measurements, and deliberately parked
rather than acted on. Phase 9 already recorded that "a normal Sweep's short bin
dwell can miss genuine traffic"; what Workstream 12 adds is the quantitative
form of that statement, plus a fixture capable of testing it.

What transfers from Workstream 12's evidence
([2026-09-04-phase12-focus-matrix.md](hardware-results/2026-09-04-phase12-focus-matrix.md)):

- Shorter bursts have fewer chances to overlap sparse samples. The 100 ms /
  six-sample Focus arm failed the tested activity rule at both measured signal
  levels; this does not establish a universal minimum sample count or require
  every detectable burst to exceed sample spacing.
- A median-relative sample count outperformed the tested summary statistics
  under those fixture conditions. It did not establish a field-independent
  activity rule. Pass A's own average/peak threshold needs separate testing.
- Sweep's four samples per bin are not equivalent to continuous observation.
  Evaluate actual spacing, receiver bandwidth, retune settling, and source
  timing together rather than transferring a Focus threshold by analogy.

What that does **not** establish, and why this is a candidate rather than a
finding:

- **Sweep is aimed at persistent energy, not packets.** Short per-bin dwells
  are a reasonable trade when covering 200+ bins, and Pass B's CAD step exists
  precisely because a Pass-A energy peak is not packet evidence. The
  architecture already encodes the distinction Workstream 12 measured.
- **One link is not a baseline.** Workstream 12 drew three conclusions from a
  single link; two held and the most confident one inverted on a second. Any
  claim about Sweep's sensitivity needs the same two-baseline discipline
  before it is written down.
- Waterfall is downstream of whatever Sweep's sampling delivers, so it is not
  a separate question. Its own risk is presentational: a cell empty because
  nothing transmitted looks identical to a cell empty because the pass did not
  sample long enough — the §3 "absence is not silence" trap in a new surface.

Entry would need: a controlled sensitivity measurement of Pass A's per-bin
sampling at two or more signal levels, reusing Workstream 12's transmitter
fixture and `benchSweepFloorQuery`'s existing per-bin floor readback; and a
decision about whether Waterfall should distinguish "sampled with no qualifying observation"
from "insufficiently sampled" in what it draws.

Rigorously sourced region packs are later candidates, not Workstream 16 and
not V2.0 blockers. Each proposed pack needs a separate entry gate with source
quality, regulatory/range rationale, fixed-table validation, and realistic
hardware access.

## Workstream 12 closeout and next entry

Workstream 12 closed in v1.1.0 on 2026-09-07. Its production surface runs one
selected bin per explicit request, uses a fixed histogram, writes `focus.csv`,
and reports coverage without activity. See `focus_coverage.h` for the shipped
thresholds and [the design-entry record](research/phase12-survey-truth-design.md)
for the measurements and decisions. Earlier prototype descriptions in that
record are historical stages, not the current release state.

Portland field validation remains deferred by operator decision. The
radio-away-budget exception applies only to one-Enter-one-pass Focus; Workstream
13 must settle the policy before introducing scheduling. Workstream 17 is in
Design entry. The [v1.1.0 audit](research/2026-09-07-v1.1.0-v2-audit.md)
records newly found defects and proposed entry-gate improvements; workstream
closure does not imply those defects are resolved.

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
