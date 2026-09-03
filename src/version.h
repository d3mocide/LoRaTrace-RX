#pragma once
// LoRaTrace RX — firmware version. Single source of truth for the boot
// banner, the on-device UI, and release tagging.
//
// TWO different things live here, deliberately kept separate:
//
// 1. FIRMWARE_VERSION — the semantic version, bumped BY HAND. MAJOR.MINOR
//    tracks the build-order phase reached (docs/ROADMAP.md Versioning); PATCH is
//    for fixes that add no phase scope. This is a *statement* that a phase
//    was reached, so it must not auto-increment — a number that changes on
//    every build asserts nothing.
//
//    **Bump this when a phase lands, and before pushing the matching
//    `vX.Y.Z` tag.** CI enforces the tag/version match in release.yml, so a
//    mismatch fails the release rather than shipping a mislabelled binary.
//
// 2. FIRMWARE_BUILD_REV — the git revision, injected automatically at build
//    time by scripts/build_rev.py. This is what actually identifies a
//    build during hardware testing, where a dozen `dev-latest` binaries can
//    share one semantic version. Carries a "-dirty" suffix when built from
//    a modified working tree.

// 0.9.0: Phase 9 (ENERGY_SWEEP/"Sweep") reached -- all five ROADMAP.md
// exit criteria closed 2026-09-03: timing/home-away duration measured;
// injected low/mid/high carriers confirmed landing in the correct
// energy.csv bin; quiet-band behavior characterized with WiFi off/on;
// CAD empirically confirmed never promoting energy alone to a Detection
// (off_grid only ever set after a real decoded packet); and two full
// 8-hour endurance soaks (docs/STATUS.md), the first of which caught and
// the second of which confirmed the fix for a real, 100%-reproducible
// logger_task stack overflow (5120->8192) plus a proactive radio_task
// stack margin fix (4096->6144) found the same way from real session.csv
// data, not guessed at. MINOR, not PATCH, since Phase 9 -- not Phase 11
// (Cell) -- is what actually landed here; see the 0.8.6-0.8.9 PATCH
// bumps below for why those stayed PATCH while Phase 9 was still open.
#define FIRMWARE_VERSION "0.9.0"

// 0.8.8: Cell's frequency bar now labels the FCC's own 869-894MHz A/B
// channel-block split (47 CFR S 22.905, cell_plan.h's CELL_BAND_BLOCKS) --
// regulatory block letter only, never a carrier name. Still Phase 11 scope
// (docs/ROADMAP.md), so PATCH again, same reasoning as the 0.8.6/0.8.7
// bumps below.

// 0.8.7: Sweep's R-key repeat toggle is now page-gated (Sweep card only,
// was global) and Cell gained the same repeat mode; Probe still has none
// (operator decision: "Repeat only on the Sweeps"). Still Phase 11 scope
// (docs/ROADMAP.md), so PATCH again, same reasoning as the 0.8.6 bump below.

// 0.8.6: Cell (869-894MHz RSSI presence sweep) added as a bounded
// radio-owned action alongside Probe/Sweep — see docs/ROADMAP.md's
// out-of-sequence "Phase 11" entry for why this is a PATCH bump, not a MINOR
// one: it is not the next build-order phase (Phase 9/ENERGY_SWEEP is still
// in progress), so bumping MINOR would misrepresent Phase 9/10 as reached.

// Fallback for builds that bypass the PlatformIO extra_script (e.g. the
// host-native test env, or an IDE indexer). Never seen on a real firmware
// build — if this string reaches a device, the script didn't run.
#ifndef FIRMWARE_BUILD_REV
#define FIRMWARE_BUILD_REV "unknown"
#endif
