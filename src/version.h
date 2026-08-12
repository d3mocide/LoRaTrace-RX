#pragma once
// LoRaTrace RX — firmware version. Single source of truth for the boot
// banner and release tagging.
//
// Bump this BEFORE pushing the matching `vX.Y.Z` git tag that triggers
// .github/workflows/release.yml — CI does not cross-check the two.
// Scheme: MAJOR.MINOR tracks the build-order phase reached (ROADMAP.md
// Versioning section), PATCH is for fixes that don't add new phase scope.

#define FIRMWARE_VERSION "0.1.0"
