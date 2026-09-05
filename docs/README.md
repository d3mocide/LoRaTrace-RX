# Documentation index

Start with the root [README.md](../README.md) for what this project is,
how to build it, and how to install it. Everything below is reference
material once you're past that.

## Read these

- **[STATUS.md](STATUS.md)** — where the project actually is right now:
  current version, what's hardware-verified, what's still open.
- **[DESIGN.md](DESIGN.md)** — shipped v1 hardware, RF parameters, and
  architecture rationale. Read before changing those foundations.
- **[ROADMAP.md](ROADMAP.md)** — active V2 workstreams, gates, and release
  policy. Start here for the next implementation decision.
- **[research/V2_DESIGN.md](research/V2_DESIGN.md)** — V2 product direction,
  permanent boundaries, and rationale behind the active roadmap.
- **[LOG_GUIDE.md](LOG_GUIDE.md)** — operator guide to run folders, CSV
  fields, identity observations, health checks, and privacy-aware export.
- **[HARDWARE_TESTING.md](HARDWARE_TESTING.md)** — repeatable
  device-validation matrix and Phase 7 memory acceptance rules.
- **[BRAND.md](BRAND.md)** — naming, tone, and on-device UI copy
  conventions.

## Working / archival material

Not onboarding reading — these are raw evidence and in-progress notes,
kept for reference rather than written to be read start to finish.

- **`history/`** — the immutable v1.0.7 roadmap snapshot pointer
  ([`ROADMAP_V1.md`](history/ROADMAP_V1.md)), plus `PROGRESS.md` and
  `CHANGELOG.md` as they stood before the 2026-08-29 documentation
  restructuring. Search history for a specific date/topic rather than
  reading it front to back; `STATUS.md` above and the root `CHANGELOG.md`
  are what stay current going forward.
- **`research/`** — design/investigation notes written during specific
  phases (e.g. Phase 8/9 sweep design), not maintained after the phase
  they were written for.
- **`hardware-results/`** — dated bench/field test result reports.
  `hardware-results/private/` (git-ignored) holds raw device captures
  that may contain precise GPS locations; only redacted aggregate
  summaries are committed.

For project rules that govern how AI coding agents work in this repo, see
the root [CLAUDE.md](../CLAUDE.md) (and [AGENTS.md](../AGENTS.md), which
just points at it).
