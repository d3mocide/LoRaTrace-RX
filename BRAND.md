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

**Revised 2026-08-25**, during the Phase 6 on-device UI redesign: Meshtastic and
MeshCore are close enough in kind — both mesh networks, picked from the same
menu group, switched between at the same control — that they read better as
one branded profile family with two sub-profiles than as two competing
top-level names. **Mesh Trace** is now that family's name; **Meshtastic** and
**MeshCore** are its plain (unbranded) sub-profile names, selected directly
from a "Mesh Trace" menu group rather than cycled one-at-a-time. **Core
Trace is retired** — nothing should introduce it as a label going forward.
Open Trace and Spectrum Trace are unaffected: Reticulum and General
Exploration are each a single profile with no sub-choice, so they keep
their own top-level name.

| Design concept | UI label |
|---|---|
| Mesh Trace profile family (Meshtastic + MeshCore) | Mesh Trace |
| — Meshtastic sub-profile | Meshtastic |
| — MeshCore sub-profile | MeshCore |
| Reticulum profile | Open Trace |
| General exploration profile | Spectrum Trace |
| HOME_LISTEN | Watch |
| DISCOVERY_SWEEP | Probe |
| ENERGY_SWEEP | Sweep |
| Saved session or run | Trace |

Deliberately **not** called a "mode" in documentation or code, even though
it's a natural word for it in conversation — HOME_LISTEN/DISCOVERY_SWEEP/
ENERGY_SWEEP already own "mode" (Watch/Probe/Sweep) for the radio's own
operating state, and Mesh Trace is a different axis (which network family
is being traced, not what the radio is currently doing to trace it).
Overloading the word would make the two impossible to talk about
separately. Where a UI string needs to show both at once (a persistent
status line, say), compose them explicitly rather than inventing a third
word for the pair — e.g. "Mesh Trace: Meshtastic," a plain colon rather
than a typographic separator, since the on-device bitmap font is ASCII
only.

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
PROFILE  Spectrum Trace
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
