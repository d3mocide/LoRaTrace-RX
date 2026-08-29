#pragma once
// LoRaTrace RX — on-device UI display labels (Phase 6: UI architecture
// redesign, ROADMAP.md; BRAND.md "Interface Naming").
//
// BRAND.md has carried a profile/mode naming table since it was written
// that the on-device UI never actually used — the header, CHANNEL page,
// and menu all printed missionProfileName()'s raw identifiers instead.
// This header is that adoption, kept deliberately separate from
// missionProfileName() (detection.h): that function's output
// (`meshtastic`, `meshcore`, ...) is what's already written into every
// `detections.csv` a real run has produced, and DESIGN.md §8 already has a
// "don't concatenate runs across a format change without checking the
// header" rule for exactly the kind of risk a cosmetic rename would create
// if the two were merged. Pure string lookups, no Arduino dependency, so
// host-testable (pio test -e native, test/test_ui_labels/) same as every
// other pure-logic header in this project.
//
// Revised twice 2026-08-25, same day (BRAND.md's own "Revised again" note
// has the full rationale): first collapsed Meshtastic/MeshCore into a
// "Mesh Trace" profile family with two sub-profiles, then walked that back
// entirely — branding every profile as its own "___ Trace" name made four
// settings on one sniffer read like four separate tools, and overloaded
// "Trace" three ways (the product name, a per-profile brand, and a saved
// session). This is the walked-back shape: one flat uiProfileLabel(),
// mapping every MissionProfile straight to its real, technical name
// (Meshtastic/MeshCore/Reticulum/Spectrum) — no family/sub-profile split,
// because there's no branded family left to split from. The on-device menu
// still *groups* these four under one "Profile" root row (ui_task.cpp's
// ROOT_ITEMS), but that's a menu-structure fact, not a label-composition
// one, so it doesn't need its own function here.

#include "channel_plans.h"
#include "discovery_plan.h"

// The plain, technical profile name — BRAND.md: no per-profile branding,
// just the real protocol name, except General Exploration, which has no
// proper noun of its own and gets a short descriptive word instead.
inline const char *uiProfileLabel(MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHTASTIC: return "Meshtastic";
        case MissionProfile::MESHCORE: return "MeshCore";
        case MissionProfile::RETICULUM: return "Reticulum";
        case MissionProfile::GENERAL_EXPLORATION: return "Spectrum";
        default: return "?";
    }
}

// BRAND.md "Interface Naming": HOME_LISTEN -> Watch. Probe is now the
// Phase 8 bounded mode; Sweep remains the future Phase 9 mode. A function
// rather than a bare string constant keeps the mode labels in one lookup
// pattern as those later radio states land.
inline const char *uiModeLabelWatch() { return "Watch"; }

// Short, screen-width-friendly candidate name for Probe's result card —
// turns "cad hit: 2" into "hit: LongModerate, ShortFast". Meshtastic
// standard presets get their real upstream preset name, keyed by slot the
// same source-backed way discovery_plan.h's own candidate table is built;
// there's no preset-name concept for MeshCore or the operator's own
// custom tuple, so those get a short descriptive word instead.
inline const char *uiDiscoveryCandidateLabel(const DiscoveryCandidate &candidate) {
    switch (candidate.kind) {
        case DiscoveryCandidateKind::MESHTASTIC_COMMUNITY_CUSTOM:
            return "Custom";
        case DiscoveryCandidateKind::MESHTASTIC_STANDARD_PRESET:
            switch (candidate.slot) {
                case 19: return "LongFast";
                case 86: return "LongModerate";
                case 26: return "LongSlow";
                case 44: return "MediumFast";
                case 51: return "MediumSlow";
                case 67: return "ShortFast";
                case 74: return "ShortSlow";
                case 13: return "LongTurbo";
                default: return "?";
            }
        case DiscoveryCandidateKind::MESHCORE_CURRENT_US:
            return "Current";
        case DiscoveryCandidateKind::MESHCORE_UPSTREAM_DEFAULT:
            return "Default";
        default:
            return "?";
    }
}
