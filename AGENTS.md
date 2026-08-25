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
- [DESIGN.md](DESIGN.md) — the "why" behind RF/architecture decisions.
- [ROADMAP.md](ROADMAP.md) — phase-by-phase build order and scope.
- [PROGRESS.md](PROGRESS.md) — current status, checklist, open questions.
- [CHANGELOG.md](CHANGELOG.md) — full session-by-session decisions log.
- [SECURITY.md](SECURITY.md) — known attack surface and how to report issues.
