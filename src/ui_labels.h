#pragma once
// LoRaTrace RX — on-device UI display labels (Phase 6: UI architecture
// redesign, ROADMAP.md; BRAND.md "Interface Naming").
//
// BRAND.md has carried a profile/mode naming table since it was written
// (Mesh Trace / Core Trace / Open Trace / Spectrum Trace; Watch / Probe /
// Sweep) that the on-device UI never actually used — the header, CHANNEL
// page, and menu all printed missionProfileName()'s raw identifiers
// instead. This header is that adoption, kept deliberately separate from
// missionProfileName() (detection.h): that function's output
// (`meshtastic`, `meshcore`, ...) is what's already written into every
// `detections.csv` a real run has produced, and DESIGN.md §8 already has a
// "don't concatenate runs across a format change without checking the
// header" rule for exactly the kind of risk a cosmetic rename would create
// if the two were merged. Pure string lookups, no Arduino dependency, so
// host-testable (pio test -e native, test/test_ui_labels/) same as every
// other pure-logic header in this project.

#include "channel_plans.h"

// BRAND.md "Interface Naming": Meshtastic -> Mesh Trace, MeshCore -> Core
// Trace, Reticulum -> Open Trace, General Exploration -> Spectrum Trace.
inline const char *uiProfileLabel(MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHTASTIC: return "Mesh Trace";
        case MissionProfile::MESHCORE: return "Core Trace";
        case MissionProfile::RETICULUM: return "Open Trace";
        case MissionProfile::GENERAL_EXPLORATION: return "Spectrum Trace";
        default: return "?";
    }
}

// BRAND.md "Interface Naming": HOME_LISTEN -> Watch. The only one of the
// three radio-mode labels (Watch / Probe / Sweep) with anything to name
// today — DISCOVERY_SWEEP ("Probe") and ENERGY_SWEEP ("Sweep") are Phase
// 7/8 (ROADMAP.md) and don't exist as radio states yet. A function rather
// than a bare string constant so callers read the same way
// uiProfileLabel() does, and so Probe/Sweep have an obvious place to join
// it once those phases land instead of needing a second lookup pattern
// invented from scratch.
inline const char *uiModeLabelWatch() { return "Watch"; }
