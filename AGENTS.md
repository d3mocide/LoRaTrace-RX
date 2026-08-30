# AGENTS.md

This project's actual working agreement for AI coding agents lives in
[CLAUDE.md](CLAUDE.md) — hardware assumptions, build system, house rules
(RX-only, sync-word sourcing, SD/heap constraints, version-bump
convention), and current project status. **Read CLAUDE.md before making
any changes here**, regardless of which agent/tool you are.

This file exists only because some tools look for `AGENTS.md` specifically
and won't discover `CLAUDE.md` on their own. It is not a second, competing
source of truth — if the two ever disagree, CLAUDE.md wins and this file
should be fixed to match, not the other way around.

Also relevant, referenced from CLAUDE.md:
- [docs/STATUS.md](docs/STATUS.md) — current status, what's
  hardware-verified, and what's still open.
- [docs/DESIGN.md](docs/DESIGN.md) — the "why" behind RF/architecture
  decisions.
- [docs/ROADMAP.md](docs/ROADMAP.md) — phase-by-phase build order and
  scope.
- [CHANGELOG.md](CHANGELOG.md) — short, ongoing changelog; full
  pre-2026-08-29 session-by-session decisions log is archived at
  [docs/history/CHANGELOG.md](docs/history/CHANGELOG.md).
- [SECURITY.md](SECURITY.md) — known attack surface and how to report issues.
- [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md) — repeatable bench
  matrix and Phase 7 memory acceptance rules.

**Don't read `docs/history/PROGRESS.md` or `docs/history/CHANGELOG.md`
end-to-end by default.** They're a frozen pre-2026-08-29 development log,
not required context for every task — `docs/STATUS.md` already gives the
current-state summary. Search them for the specific date/version/topic you
need instead of reading front to back.
