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
// Revised 2026-08-25 (BRAND.md's own "Revised" note has the full
// rationale): Meshtastic and MeshCore collapsed from two competing
// top-level names (Mesh Trace / Core Trace) into one profile family (Mesh
// Trace) with two plain sub-profile names, matching the menu's own new
// shape (Phase 6: "Mesh Trace" is a GROUP root row now, not a two-way
// cycle) — see ui_menu.h and ui_task.cpp's ROOT_ITEMS. uiProfileLabel() is
// gone; nothing needs "the one flat label" any more now that every caller
// wants either the family name, the sub-profile name, or both composed.

#include <stdio.h>

#include "channel_plans.h"

// The profile family name — BRAND.md: Meshtastic and MeshCore both answer
// "Mesh Trace"; Reticulum/General Exploration have no sub-profile, so this
// is their whole label too (see uiSubProfileLabel() below).
inline const char *uiTraceModeLabel(MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHTASTIC:
        case MissionProfile::MESHCORE: return "Mesh Trace";
        case MissionProfile::RETICULUM: return "Open Trace";
        case MissionProfile::GENERAL_EXPLORATION: return "Spectrum Trace";
        default: return "?";
    }
}

// The plain (unbranded) sub-profile within Mesh Trace — "Meshtastic" or
// "MeshCore," deliberately not "Core Trace": that name is retired
// (BRAND.md). Empty for profiles with no sub-choice, so callers can test
// for "" rather than needing a separate hasSubProfile() predicate.
inline const char *uiSubProfileLabel(MissionProfile profile) {
    switch (profile) {
        case MissionProfile::MESHTASTIC: return "Meshtastic";
        case MissionProfile::MESHCORE: return "MeshCore";
        default: return "";
    }
}

// The composed "what's actually active" string — family name alone, or
// family + sub-profile when there is one ("Mesh Trace: Meshtastic"). A
// plain colon, not a typographic separator: the on-device bitmap font is
// ASCII only, and this is the one label string in the UI wide enough to
// carry the full composition (drawFooterStatus(), ui_task.cpp) rather than
// the header's tight quarters, which only ever show the bare page name now.
inline void uiActiveProfileLabel(MissionProfile profile, char *out, size_t outSize) {
    const char *sub = uiSubProfileLabel(profile);
    if (sub[0] != '\0') {
        snprintf(out, outSize, "%s: %s", uiTraceModeLabel(profile), sub);
    } else {
        snprintf(out, outSize, "%s", uiTraceModeLabel(profile));
    }
}

// BRAND.md "Interface Naming": HOME_LISTEN -> Watch. The only one of the
// three radio-mode labels (Watch / Probe / Sweep) with anything to name
// today — DISCOVERY_SWEEP ("Probe") and ENERGY_SWEEP ("Sweep") are Phase
// 7/8 (ROADMAP.md) and don't exist as radio states yet. A function rather
// than a bare string constant so callers read the same way the labels
// above do, and so Probe/Sweep have an obvious place to join it once those
// phases land instead of needing a second lookup pattern invented from
// scratch.
inline const char *uiModeLabelWatch() { return "Watch"; }
