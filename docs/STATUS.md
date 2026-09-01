# Status

The one place "where is this project right now" lives. Replaces status
prose that used to be duplicated (and drifting) across `CLAUDE.md`,
`PROGRESS.md`, and `README.md`. For *how* we got here, see
`docs/history/CHANGELOG.md`; for the phase-by-phase build order, see
[ROADMAP.md](ROADMAP.md).

## Current version

**v0.8.9** (`src/version.h`). `MAJOR.MINOR` tracks the build-order phase
*reached*, not the phase in progress — see ROADMAP.md's Versioning
section. `v0.8.x` = Phase 8 complete; Phase 9 is underway, so the version
correctly hasn't moved to `0.9.0` yet. The PATCH bump is Phase 11 (Cell,
below) — an out-of-sequence addition, not a fix, but not the
next build-order phase either; see ROADMAP.md's Versioning section for why
that's a PATCH bump rather than a MINOR one. `v0.8.7` adds Cell's repeat
mode and page-gates Sweep's own R key (below) — still Phase 11 scope, so
another PATCH, not a MINOR. `v0.8.8` adds FCC A/B block markers to the
Cell frequency bar (below) — same reasoning, another PATCH. `v0.8.9` adds
a Sweep region setting (below, Phase 9 scope this time, not Phase 11) —
same out-of-sequence-addition reasoning, another PATCH.

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
- **Region setting (`v0.8.9`, added and hardware-verified 2026-09-01):**
  System > Region narrows Sweep's scanned band to 902-923MHz (US, the
  default) instead of the full 868-923MHz range (Global), roughly
  halving scan time — 47 CFR § 15.247 cited in `energy_plan.h`.
  Persisted to `/loratrace/region.txt`. Cell and `channel_plans.h` are
  explicitly NOT region-aware (see `docs/ROADMAP.md`'s Phase 9 follow-up
  bullets). Confirmed on real hardware (`b7c845a`): System > Region
  shows US by default and cycles to Global and back; a US-region Sweep
  shows the frequency bar reading 902/923 and completes noticeably
  faster; a Global-region Sweep reads 868/923 at full duration, matching
  pre-change behavior; the choice survives a reboot. One bug was caught
  and fixed during this verification: the System menu's row count was
  still hardcoded to 3 after Region became its 4th row, silently hiding
  the new row — `ui_task.cpp`'s `SYSTEM_GROUP_ITEMS` GROUP entry now
  correctly says 4.
- **Pass B (CAD at peaks) has not started.**

Phase 10 (Field Analyzer) is accepted as planned scope; whether it's
required before `v1.0.x` is an explicit decision deferred until Phase 9
hardware evidence exists (ROADMAP.md).

**Phase 11 (Cell) — added out of sequence, PARTIALLY hardware-verified:**
a bounded RSSI-only presence sweep of 869-894MHz (North American Cellular
downlink), operator-requested after real wardriving runs picked up energy
in that band near cell towers. It is not a decode of any kind — the SX1262
cannot demodulate GSM/CDMA/LTE — and not a fifth mission profile; same
operator surface as Probe/Sweep (global hotkey C, dedicated carousel card),
not a menu row. See `docs/ROADMAP.md`'s Phase 11 entry and `docs/DESIGN.md`
§5a for the full design and why it's numbered outside the normal phase
sequence. Code and host-native tests landed 2026-09-01; on 2026-09-01 it
was flashed to real hardware (v0.8.6, `b7c845a`) and confirmed: the C key
runs a one-shot Cell scan end-to-end (toast through `Cell: DONE` with a
real MHz/dBm reading on the carousel card, home channel restored after),
and the Probe/Sweep mutual-exclusion guard holds in both directions
(Cell during an active Sweep gives `Sweep` priority and refuses with
`Cell: UNAVAILABLE`; Sweep during an active Cell scan is refused the same
way). Still unverified: a real sweep near a known tower showing RSSI
rising above the floor, and `cell.csv`/`session.csv`'s new columns written
correctly to SD.

**Repeat mode (R), `v0.8.7`, hardware-verified 2026-09-01:** Cell gained a
repeat mode identical in shape to Sweep's own (`radioRequestCellSweepRepeat()`,
back-to-back laps with a lap counter on the card). This also changed
Sweep's own R key: it was a global hotkey (fired from anywhere, including
with the menu open); it's now page-gated by `ui_task.cpp` to the Sweep/Cell
cards specifically, matching how Enter (`SELECT`) already dispatches
per-page — a no-op on any other page or with the menu open. Probe
deliberately has no repeat mode (operator decision: "Repeat only on the
Sweeps"). Confirmed on real hardware (`b7c845a`, same session as Cell's own
verification above): Sweep-repeat still starts/stops correctly after the
page-gating change; Cell-repeat starts/stops correctly with its lap counter
rendering cleanly; R is a no-op on Probe (including mid-scan), every other
page, and with the menu open from either Sweep or Cell; and mutual
exclusion holds in both directions across a real repeat chain, not just a
single shot (Cell refused during Sweep-repeat, Sweep refused during
Cell-repeat).

**FCC A/B block markers, `v0.8.8`, hardware-verified 2026-09-01:** the Cell
frequency bar now labels the FCC's own downlink sub-band split within
869-894MHz — Block A (869-880MHz + 890-891.5MHz) and Block B (880-890MHz +
891.5-894MHz), cited to 47 CFR § 22.905 (`cell_plan.h`'s
`CELL_BAND_BLOCKS`) — as a two-shade tick row under the bar with letter
labels on the two segments wide enough to hold one. Regulatory block letter
only, deliberately no carrier name (current licensee varies by market and
isn't a fixed national fact — see `docs/DESIGN.md` §5a). Confirmed on real
hardware (`b7c845a`): the tick row renders cleanly with no overlap against
the lo/hi labels above or the disclaimer line below.

## What's still open

- **923-928MHz front-end rolloff** needs an empirical RSSI-floor
  characterization so the UI can be honest about reduced sensitivity in
  that sub-band, rather than silently under-reporting (ROADMAP.md's Phase
  9 blocking unknown).
- **Pass B** (CAD at energy peaks) design and implementation.
- **Cell hardware verification** (Phase 11, above) — C key, mutual
  exclusion against Probe/Sweep (both directions), and the carousel card
  are now confirmed on real hardware (2026-09-01). Still open: confirm a
  real cell-band RSSI reading actually rises near a known tower, and
  confirm `cell.csv`/`session.csv`'s new columns write correctly to SD.
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
