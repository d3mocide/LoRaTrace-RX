# Status

The one place "where is this project right now" lives. Replaces status
prose that used to be duplicated (and drifting) across `CLAUDE.md`,
`PROGRESS.md`, and `README.md`. For *how* we got here, see
`docs/history/CHANGELOG.md`; for the phase-by-phase build order, see
[ROADMAP.md](ROADMAP.md).

## Current version

**v0.8.6** (`src/version.h`). `MAJOR.MINOR` tracks the build-order phase
*reached*, not the phase in progress — see ROADMAP.md's Versioning
section. `v0.8.x` = Phase 8 complete; Phase 9 is underway, so the version
correctly hasn't moved to `0.9.0` yet. The PATCH bump is Phase 11 (Cell
Tower Trace, below) — an out-of-sequence addition, not a fix, but not the
next build-order phase either; see ROADMAP.md's Versioning section for why
that's a PATCH bump rather than a MINOR one.

## What's hardware-verified

Phases 0-8 are complete and hardware-verified: radio bring-up (Phase 1),
the task/queue architecture + GPS + SD logging that makes up MVP-Beta
(Phase 2), the WiFi AP + web command center (Phase 3), the MeshCore
profile and live profile switch (Phase 4), the on-device menu UI
(Phase 5), the UI architecture redesign (Phase 6), measured heap/stack
budgets and soak (Phase 7 — its strict same-build repetition criterion
was explicitly waived for that cycle, see `docs/history/PROGRESS.md`),
and bounded radio-owned discovery scanning with a source-backed candidate
plan (Phase 8, `DISCOVERY_SWEEP` / "Probe"). Phase 8's statistical CAD
false/miss matrix remains an explicit lab follow-up — the available bench
can't provide a known-quiet RF control.

**Phase 9 (`ENERGY_SWEEP` / "Sweep") is in progress:**
- Pass A (frequency-binned energy acquisition across 868-923MHz) is
  hardware-verified end-to-end: real `energy.csv` peak rows spanning the
  full band, zero queue drops, clean home-restore across repeated sweeps.
- Serial Control (`SWEEP_START`/`SWEEP_CANCEL`/`STATUS`) and a dedicated
  on-device Sweep result card are wired and hardware-verified.
- The noise-floor margin is calibrated from a bench-run matrix: the
  shipped default moved from a 10.0dB placeholder to a measured 35.0dB,
  re-verified at 0/221 false peaks across three consecutive production
  sweeps.
- **Pass B (CAD at peaks) has not started.**

Phase 10 (Field Analyzer) is accepted as planned scope; whether it's
required before `v1.0.x` is an explicit decision deferred until Phase 9
hardware evidence exists (ROADMAP.md).

**Phase 11 (Cell Tower Trace) — added out of sequence, NOT hardware-verified:**
a bounded RSSI-only presence sweep of 869-894MHz (North American Cellular
downlink), operator-requested after real wardriving runs picked up energy
in that band near cell towers. It is not a decode of any kind — the SX1262
cannot demodulate GSM/CDMA/LTE — and not a fifth mission profile; see
`docs/ROADMAP.md`'s Phase 11 entry and `docs/DESIGN.md` §5a for the full
design and why it's numbered outside the normal phase sequence. Code and
host-native tests landed 2026-09-01; this was implemented without bench
access to the physical device, so treat it as unverified until a real
sweep near a known tower is confirmed on hardware to actually show RSSI
rising above the floor, and the Probe/Sweep mutual-exclusion guards are
confirmed to hold.

## What's still open

- **923-928MHz front-end rolloff** needs an empirical RSSI-floor
  characterization so the UI can be honest about reduced sensitivity in
  that sub-band, rather than silently under-reporting (ROADMAP.md's Phase
  9 blocking unknown).
- **Pass B** (CAD at energy peaks) design and implementation.
- **Cell Trace hardware verification** (Phase 11, above) — confirm a real
  cell-band RSSI reading actually rises near a known tower, confirm the
  mutual-exclusion guards against Probe/Sweep on real hardware, confirm
  `cell.csv`/`session.csv`'s new columns render correctly.
- Phase 9's full exit criteria — timing/home-away duration measurement,
  injected low/mid/high carriers landing in the correct bins, quiet-band
  characterization with WiFi off/on, CAD never promoting energy alone to
  LoRa, and a 24-hour repeated-sweep soak — are not all closed yet.

`docs/history/PROGRESS.md` has a much older "Open questions" list dating
back to Phases 1-2 (2026-08-22 through 2026-08-27). Most of those are
resolved and marked `[x]` there; a few unresolved ones (MeshCore's
encryption/PSK scheme, CAD `symNum` tuning, Meshtastic's exact per-slot US
frequency table) were never revisited after that file stopped being the
live status doc — treat them as leads to re-check, not confirmed-current
open items.

## Three hard-won hardware facts

Kept here as well as in `CLAUDE.md` (agents read that file, humans are
more likely to land here first):

- **The IO expander's P0 powers the GPS as well as switching the RF
  antenna path.** A "dead" GPS or a silent radio is often just this.
- **A wrong sync word is silent, not loud** — the radio simply never
  interrupts, while still hearing unrelated traffic that matches.
- **A same-node-id detection pair with wildly different RSSI seconds
  apart is very likely a genuine mesh relay, not a logging bug** —
  `packet_id`/`hop_limit`/`hop_start`/`relay_node` in `detections.csv`
  prove it either way.
