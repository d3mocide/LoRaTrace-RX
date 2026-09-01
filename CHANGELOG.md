# Changelog

Full pre-2026-08-29 history (session-by-session development log, every
decision and debugging story) is archived unedited at
[docs/history/CHANGELOG.md](docs/history/CHANGELOG.md). This file starts
fresh from the documentation restructuring below and stays terse —
one or two lines per entry, newest first. For the current state of the
project (not a log of how it got there), see [docs/STATUS.md](docs/STATUS.md).

## 2026-09-01

- Added Cell (`v0.8.6`): a bounded RSSI-only presence sweep of
  869-894MHz (North American Cellular downlink), operator-requested after
  real wardriving runs picked up energy in that band near cell towers. Not
  a decode of any kind (the SX1262 cannot demodulate GSM/CDMA/LTE) and not
  a fifth mission profile — a third bounded radio-owned action alongside
  Probe/Sweep (own global hotkey C and carousel card, same as those two),
  isolated from `ENERGY_SWEEP`'s calibrated Pass A/B engine.
  See `docs/ROADMAP.md`'s Phase 11 entry and `docs/DESIGN.md` §5a. Code +
  host-native tests only — not yet hardware-verified (`docs/STATUS.md`).

## 2026-08-29

- Restructured docs: moved `DESIGN.md`, `ROADMAP.md`, `LOG_GUIDE.md`,
  `HARDWARE_TESTING.md`, `BRAND.md`, `research/`, and `hardware-results/`
  into `docs/`; archived `PROGRESS.md` and the prior `CHANGELOG.md`
  verbatim to `docs/history/`; added `docs/STATUS.md` as the single
  current-status doc (replacing duplicated status prose that had drifted
  across `CLAUDE.md`, `PROGRESS.md`, and `README.md`) and `docs/README.md`
  as a docs index.
