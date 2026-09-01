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

// 0.8.9: System > Region narrows Sweep's scanned band to 902-923MHz (US,
// the default) instead of the full 868-923MHz range (Global) -- 47 CFR S
// 15.247's US ISM floor, region_plan.h/energy_plan.h. Out-of-sequence
// Phase 9 scope (not Phase 11 this time), so PATCH again, same reasoning
// as the 0.8.6/0.8.7/0.8.8 bumps below.
#define FIRMWARE_VERSION "0.8.9"

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
