# Phase 8 discovery research

Status: research baseline and built-in plan foundation, 2026-08-27.

## Findings

### Meshtastic channel tuples

Meshtastic's current firmware uses a 902.0-928.0 MHz US region and computes
the number of frequency slots from the selected bandwidth. The selected slot
is the channel-name hash modulo that count, and the frequency is the region
start plus half the channel bandwidth plus the slot spacing. The current
firmware also defines LongFast as SF11, 250 kHz, CR 4/5 and uses sync word
`0x2B`.

This means a discovery plan cannot treat the US band as one universal
104-slot/250-kHz grid. The built-in Phase 8 plan therefore contains eight
supported standard Meshtastic modem-preset anchors, each with its own
source-derived bandwidth, slot count, slot, frequency, SF, and coding rate.
The standard ShortTurbo anchor is omitted because its US slot centre is
926.75 MHz, outside this module's supported 868-923 MHz front-end range. The
active home tuple remains eligible in the static plan so the radio acquisition
layer can skip the resolved home configuration consistently, including a
per-profile override.

### MeshCore tuples

MeshCore does not use Meshtastic's channel-name slot hash. The upstream
companion example exposes its default radio tuple as 915.0 MHz, 250 kHz,
SF10, CR 4/5. The existing LoRaTrace-RX US narrow-band home tuple remains a
separate built-in candidate. No pre-migration/legacy MeshCore tuple was added:
the project's evidence rules do not permit guessing its coding rate.

### CAD and bounded execution

Semtech describes SX126x CAD as a LoRa preamble/activity detector and the
official Meshtastic radio interface uses two CAD symbols. Local RadioLib
7.7.1 exposes the needed split API (`startChannelScan()` and
`getChannelScanResult()`), but its blocking `scanChannel()` waits on the IRQ
pin without a software deadline. Phase 8 acquisition must therefore use the
split API inside the radio task, poll with an explicit deadline, and always
restore Watch's resolved home tuple on completion, cancel, timeout, or
failure. The implementation must not call the blocking helper.

## Decisions for this implementation slice

- Built-in plans are fixed, versioned, and compiled from source-backed data.
- Persistent custom candidate editing remains post-Phase-8 scope.
- Reticulum and General Exploration remain empty here; they are Phase 9
  energy-sweep consumers, not fixed discovery plans.
- The plan layer is pure and allocation-free. The first integration slice now
  adds radio-owned bounded CAD acquisition, a separate fixed observation queue,
  packet-bearing receive-on-hit, home restoration, and durable `probe.csv`.
  Transient mode and the deterministic cancellation/fault-injection harness
  remain open.

## Open validation gates

- Bench-test CAD `symNum` and false-positive/miss behavior against a known
  alternate transmitter. The initial two-symbol choice is source-backed but
  is not yet this device's measured optimum.
- Bench-verify the radio-owned bounded acquisition state machine and its
  separate fixed `ScanObservation` queue; packet hits must continue through
  `Detection`.
- Add deterministic cancellation/fault-injection tests and the 1,000-cycle
  automated bench mode required by `ROADMAP.md`.

## Primary sources

- Meshtastic `RadioInterface.cpp`: region, slot-count/hash, and frequency
  formula: https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioInterface.cpp
- Meshtastic `RadioInterface.h`: LongFast radio parameters and two-symbol CAD
  setting: https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioInterface.h
- Meshtastic `RadioLibInterface.h`: current sync word `0x2B`:
  https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioLibInterface.h
- MeshCore `MyMesh.h`: upstream companion default radio tuple:
  https://github.com/meshcore-dev/MeshCore/blob/main/examples/companion_radio/MyMesh.h
- Semtech SX1262 product resources, including AN1200.48:
  https://www.semtech.com/products/wireless-rf/lora-connect/sx1262
- Semtech CAD FAQ: https://www.semtech.com/design-support/faq/faq-lora/P40
- RadioLib SX126x implementation/API:
  https://github.com/jgromes/RadioLib/blob/master/src/modules/SX126x/SX126x.cpp
  and https://github.com/jgromes/RadioLib/blob/master/src/modules/SX126x/SX126x.h
- RadioLib issue documenting the blocking-CAD hang risk:
  https://github.com/jgromes/RadioLib/issues/1830
