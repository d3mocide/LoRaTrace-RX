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

// BRAND.md "Interface Naming": HOME_LISTEN -> Watch. The only one of the
// three radio-mode labels (Watch / Probe / Sweep) with anything to name
// today — DISCOVERY_SWEEP ("Probe") and ENERGY_SWEEP ("Sweep") are Phase
// 7/8 (ROADMAP.md) and don't exist as radio states yet. A function rather
// than a bare string constant so callers read the same way the label
// above does, and so Probe/Sweep have an obvious place to join it once
// those phases land instead of needing a second lookup pattern invented
// from scratch.
inline const char *uiModeLabelWatch() { return "Watch"; }
