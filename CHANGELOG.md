# Changelog

Full pre-2026-08-29 history (session-by-session development log, every
decision and debugging story) is archived unedited at
[docs/history/CHANGELOG.md](docs/history/CHANGELOG.md). This file starts
fresh from the documentation restructuring below and stays terse —
one or two lines per entry, newest first. For the current state of the
project (not a log of how it got there), see [docs/STATUS.md](docs/STATUS.md).

## 2026-09-01

- Added a Sweep region setting (`v0.8.9`): System > Region narrows
  Sweep's scanned band to 902-923MHz (US, the default) instead of the
  full 868-923MHz hardware range (Global), cited to 47 CFR § 15.247,
  roughly halving scan time for the common case. Persisted to
  `/loratrace/region.txt`. Cell and `channel_plans.h` are explicitly not
  region-aware yet — noted as separate follow-ups in `docs/ROADMAP.md`.
  Code + host-native tests only — not yet hardware-verified
  (`docs/STATUS.md`).
- Added FCC A/B block markers to the Cell frequency bar (`v0.8.8`): labels
  the FCC's own 869-880/890-891.5MHz (Block A) and 880-890/891.5-894MHz
  (Block B) split, cited to 47 CFR § 22.905. Regulatory block letter only
  — deliberately no carrier name, since actual current licensee varies by
  market and isn't a fixed national fact. Code + host-native tests only —
  not yet hardware-verified (`docs/STATUS.md`).
- Made R's repeat toggle page-gated (`v0.8.7`): it now only acts on the
  Sweep or Cell card (no-op elsewhere, including with the menu open),
  instead of firing globally like it did for Sweep alone. Cell gained the
  same repeat mode Sweep already had; Probe still has none by operator
  decision ("Repeat only on the Sweeps"). Hardware-verified same day —
  page-gating, Cell's lap counter, and mutual exclusion across a real
  repeat chain all confirmed (`docs/STATUS.md`).
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
