# LoRaTrace Brand Guide

## Brand Overview

**LoRaTrace** is a concise, technically legible name for an RX-only field tool that detects, classifies, GPS-tags, and records LoRa and broader sub-GHz radio activity. The name works because “LoRa” provides immediate RF context, while “Trace” suggests evidence, logging, and geospatial observation rather than active participation in a network.[1]

The brand should present the project as a passive field instrument, not a general-purpose offensive security tool. That framing fits the design direction of receive-only discovery, protocol fingerprinting, and location-aware logging across known-channel and discovery-oriented mission profiles.[1]

## Naming Architecture

The primary public-facing project name should be **LoRaTrace**. A more explicit technical label such as **LoRaTrace RX** works well for firmware banners, binaries, boot screens, and repository naming because it makes the receive-only nature clear without cluttering the main brand.[1]

Recommended naming layers:

- **Project name:** LoRaTrace
- **Technical variant:** LoRaTrace RX
- **Repository name:** `loratrace-rx`
- **Compact mark:** `LTRX`
- **Log namespace:** `/loratrace/`
- **Config prefix:** `ltrx`

This structure keeps the human-readable brand clean while giving build artifacts and code-facing surfaces unambiguous technical identifiers.

## Brand Meaning

“Trace” is a stronger fit than “Track” for this product because the firmware is not following a single network, node, or authenticated session. Instead, it records observed radio events with associated metadata such as signal characteristics, inferred protocol, timestamp, and GPS position.[1]

That distinction matters for both clarity and tone. “Track” can imply active following or persistent monitoring of a specific target, while “Trace” better communicates passive collection and later analysis of RF evidence.[1]

## Positioning

LoRaTrace should be positioned as a **passive LoRa and sub-GHz field logging** platform. It is best described as a field recorder for radio observations rather than a mesh client, mapper, or packet injector.[1]

Recommended positioning statement:

> LoRaTrace is a receive-only field logging platform for LoRa and sub-GHz signal discovery, classification, and GPS-tagged observation.[1]

Supporting positioning themes:

- Passive by design
- GPS-tagged RF observations
- Protocol-aware but protocol-agnostic at the platform level
- Useful for Meshtastic, MeshCore, Reticulum candidate discovery, and general spectrum exploration
- Built like a field instrument, not a consumer app

## Taglines

Recommended primary tagline:

**Passive LoRa and sub-GHz field logging**

Additional approved tagline options:

- GPS-tagged RF discovery for LoRa and sub-GHz
- Receive-only detection, classification, and field mapping
- A field recorder for LoRa, mesh, and unknown sub-GHz activity
- Follow the signal, not the network
- Every signal leaves a trace

These lines preserve the brand’s passive identity while leaving room for broader sub-GHz discovery beyond only LoRa-marked traffic.[1]

## Voice and Tone

The brand voice should be calm, precise, and instrument-like. It should sound closer to a survey receiver, spectrum tool, or field logger than to a red-team utility or hacker toy.

Preferred voice attributes:

- Technical, not theatrical
- Confident, not aggressive
- Clear, not mysterious
- Field-operator oriented
- Concise and data-driven

Preferred language examples:

- “Observation” instead of “target”
- “Detection” instead of “hit” when formal UI text is needed
- “Profile” instead of “attack mode”
- “Trace” or “session” instead of “hunt” in saved-log contexts

## Visual Direction

The visual identity should follow a restrained field-instrument aesthetic. Dark charcoal or near-black surfaces, muted green for GPS/location emphasis, and amber for detections fit the product better than neon palettes or spooky ghost-themed artwork.

Recommended visual cues:

- Dark neutral background with high-contrast readable text
- Muted GPS/terrain green as the primary accent
- Amber or yellow for event and detection highlights
- Compact monospace or technical sans-serif UI typography
- Minimal ornamentation, with emphasis on status clarity and signal data density

The compact mark can be **LTRX**, or a symbol derived from a route line, map pin, and RF arcs. A useful logo concept is an L-shaped path that transitions into three signal arcs, reinforcing the combination of movement, location, and radio observation.

## Interface Naming

UI labels can extend the brand while staying readable on-device.

**Revised again 2026-08-25**, same day, walking back the revision directly
above this note: branding every profile as its own "___ Trace" name (Mesh
Trace, Core Trace, Open Trace, Spectrum Trace) overloaded "Trace" three
ways at once — the product name (LoRaTrace), a per-profile brand, and a
saved session — and made four settings on one sniffer read like four
separate tools. LoRaTrace is one receiver; Meshtastic/MeshCore/Reticulum/
General Exploration are LoRa presets it can be pointed at, not sibling
products. **Profile** replaces all four "___ Trace" names as the one word
for that axis — not a new coinage, it's already this doc's own preferred
term ("Voice and Tone" below: "'Profile' instead of 'attack mode'"); the
Trace-branding detour was overriding vocabulary this doc already had.
Presets keep their real, technical names instead of marketed ones —
**Meshtastic**, **MeshCore**, **Reticulum**, and **Spectrum** (short for
General Exploration, the one profile without its own proper noun to fall
back on). **Trace now means exactly one thing: a saved session or run** —
"start a Trace," "your saved Traces" — the meaning it always had underneath
the branding, now the only one left standing.

**Exception carved out later the same day:** the Phase 6 UI bench pass
added a pause/standby toggle for the radio-listening + logging pipeline,
and it also uses the word "Trace" — a root-level menu row reading "Trace:
Active"/"Trace: Standby". That's a second meaning for the word, which is
exactly what the walk-back above was written to stop happening. It was
kept anyway, deliberately, rather than renamed to "Listen"/"RX"/etc.,
because the two meanings don't actually collide in practice: **the saved-
session noun is always a countable thing** ("a Trace," "your saved
Traces," a row in the run browser) **while the live-toggle usage is always
paired with a state word** (Active/Standby) and never stands alone as a
noun. A reader never has to disambiguate "which Trace" the way they would
have had to disambiguate "which Mesh Trace" under the branding this doc
already rejected once. Still worth being honest that this is a narrower
exception to "exactly one thing," not a case where the rule turned out not
to apply.

| Design concept | UI label |
|---|---|
| Profile selector (on-device menu group) | Profile |
| — Meshtastic profile | Meshtastic |
| — MeshCore profile | MeshCore |
| — Reticulum profile | Reticulum |
| — General Exploration profile | Spectrum |
| HOME_LISTEN | Watch |
| DISCOVERY_SWEEP | Probe |
| ENERGY_SWEEP | Sweep |
| Saved session or run | Trace |
| Radio-listening pause/standby (on-device toggle) | Trace: Active / Trace: Standby |

"Profile" and "mode" stay two different axes, same reasoning as before this
revision: HOME_LISTEN/DISCOVERY_SWEEP/ENERGY_SWEEP own "mode" (Watch/Probe/
Sweep) for what the radio is currently doing; Profile is which LoRa preset
it's doing that with. A persistent status line showing both composes them
plainly — "Meshtastic," "Watch" — no combined label needed now that neither
side carries brand text of its own.

These labels align well with the project name and keep the interface compact for a small embedded display.

## Example Usage

Example firmware banner:

```text
LoRaTrace RX v0.1
```

Example repository description:

```text
Passive LoRa and sub-GHz field logging for GPS-tagged RF discovery and protocol-aware observation.
```

Example on-device screen copy:

```text
LoRaTrace RX
PROFILE  Spectrum
MODE     Sweep
GPS      45.5231, -122.6765
EVENTS   0142
```

## Guardrails

The brand should avoid spooky, militarized, or overtly adversarial language. Names, visuals, and copy should emphasize receive-only observation, logging, surveying, and analysis rather than interception or intrusion.

Avoid:

- Ghost or haunt-heavy identity systems
- Neon cyberpunk color schemes
- “Weaponized” security language
- Implied transmit, inject, or attack framing
- Overly consumerized app-store aesthetics

Prefer:

- Survey and instrumentation metaphors
- Technical clarity
- Geographic and RF context
- Compact operator-focused interfaces
- Durable, repo-friendly naming

## Recommended Standard

The recommended brand standard is:

- **Project name:** LoRaTrace
- **Technical name:** LoRaTrace RX
- **Short descriptor:** Passive LoRa and sub-GHz field logging
- **Repository name:** `loratrace-rx`
- **Mark:** `LTRX`

This naming system gives the project a clean public identity while preserving enough specificity for firmware, documentation, and future expansion into broader sub-GHz exploration workflows.[1]
