#pragma once
// LoRaTrace RX — firmware version. Single source of truth for the boot
// banner, the on-device UI, and release tagging.
//
// TWO different things live here, deliberately kept separate:
//
// 1. FIRMWARE_VERSION — the semantic version, bumped BY HAND. MAJOR.MINOR
//    tracks the build-order phase reached (ROADMAP.md Versioning); PATCH is
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

#define FIRMWARE_VERSION "0.2.7"

// Fallback for builds that bypass the PlatformIO extra_script (e.g. the
// host-native test env, or an IDE indexer). Never seen on a real firmware
// build — if this string reaches a device, the script didn't run.
#ifndef FIRMWARE_BUILD_REV
#define FIRMWARE_BUILD_REV "unknown"
#endif
