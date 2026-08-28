# Phase 8 discovery research

Status: source set rechecked and initial candidate-specific CAD correlation
captured, 2026-08-28. This is not a statistical false-positive/miss-rate gate.

## Findings

### Meshtastic channel tuples

Meshtastic's current firmware uses a 902.0-928.0 MHz US region and computes
the number of frequency slots from the selected bandwidth. The selected slot
is the channel-name hash modulo that count, and the frequency is the region
start plus half the channel bandwidth plus the slot spacing. The current
firmware also defines LongFast as SF11, 250 kHz, CR 4/5 and uses sync word
`0x2B`.

This means a discovery plan cannot treat the US band as one universal
104-slot/250-kHz grid, nor enumerate all custom channel hashes. The built-in
Phase 8 plan is therefore deliberately limited to eight fixed, source-bounded
standard-preset anchors, each with its own bandwidth, slot count, slot,
frequency, SF, and coding rate. A known deployment remains an explicit
per-profile home override; it is not silently guessed as a universal Probe
candidate.
The standard ShortTurbo anchor is omitted because its US slot centre is
926.75 MHz, outside this module's supported 868-923 MHz front-end range. The
active home tuple remains eligible in the static plan so the radio acquisition
layer can skip the resolved home configuration consistently, including a
per-profile override.

### Operator-supplied MeshOregon tuple

For this local bench, the operator supplied the current MeshOregon physical
settings: override frequency 918.5 MHz, SF8, BW125 kHz, CR 4/5, and the
normal Meshtastic sync word. Channel names and PSKs deliberately do not enter
the occupancy plan: they are not modem parameters needed for CAD, and Probe
does not decode or handle payload keys. This one community-specific tuple is
versioned in the Meshtastic plan and placed first, so the locally requested
candidate is sampled before unrelated CAD activity can consume a bounded
receive-on-hit window. It is not presented as a universal Meshtastic default
or a replacement for a sourced channel-hash plan.

### MeshCore tuples

MeshCore does not use Meshtastic's channel-name slot hash. The upstream
companion example exposes its default radio tuple as 915.0 MHz, 250 kHz,
SF10, CR 4/5. MeshCore's upstream FAQ separately recommends the USA/Canada
narrow tuple, 910.525 MHz, SF7, BW62.5, CR5; that is the existing LoRaTrace-RX
home tuple and the first fixed candidate. CascadiaMesh's local operator
guidance directs its Oregon/Washington/British Columbia network to the same
USA/Canada preset and explicitly identifies 910.525 MHz as its frequency;
that real-world local relevance is why it precedes the upstream 915 MHz
fallback. No pre-migration SF11/250 legacy tuple was added: its coding rate is
not established, and the project's evidence rules do not permit guessing it.

### Candidate-plan acceptance

Discovery-plan version 2 is the complete bounded catalogue for Phase 8's two
implemented profiles, not a claim to enumerate arbitrary custom networks:

| Profile | Ordered candidates | Provenance and scope |
|---|---|---|
| Meshtastic | Local MeshOregon override, then the eight documented US standard-preset anchors inside the 868–923 MHz front-end range | Upstream preset/slot rules; the local tuple is operator-supplied and labelled community-specific. ShortTurbo is outside the supported front-end range. |
| MeshCore | USA/Canada narrow, then upstream companion default | The first candidate is weighted by Cascadia's local guidance plus MeshCore's full documented tuple. The second is exact upstream source data. |
| Reticulum / General Exploration | No fixed candidates | Phase 9 energy-first acquisition owns those profiles; a CAD hit alone cannot name a protocol. |

This accepts the plan as versioned, sourced, bounded, deduplicated data. It
does not turn a custom local tuple into a universal default, and it preserves
the per-profile home override as the operator's route to a known deployment.

### CAD and bounded execution

Semtech describes SX126x CAD as a LoRa preamble/activity detector and the
official Meshtastic radio interface uses two CAD symbols. Local RadioLib
7.7.1 exposes the needed split API (`startChannelScan()` and
`getChannelScanResult()`), but its blocking `scanChannel()` waits on the IRQ
pin without a software deadline. Phase 8 acquisition must therefore use the
split API inside the radio task, poll with an explicit deadline, and always
restore Watch's resolved home tuple on completion, cancel, timeout, or
failure. The implementation must not call the blocking helper.

### CAD rate acceptance protocol

SX1262 CAD supports 1, 2, 4, 8, or 16 symbols. The production image remains
fixed at the upstream two-symbol setting; only the compile-time-gated bench
image accepts `BENCH_CAD` to select another supported window. The matrix uses
the non-home LongModerate fixture candidate, because the active local
MeshOregon home tuple is correctly skipped by Probe.

For each window, the strict gate is 20 fixture-quiet Probes followed by 20
capped -9 dBm fixture pulses. Every cycle must reach `COMPLETE`, keep `SD=1`,
and restore Watch. The candidate bit must be clear in every quiet run and set
in every pulse run; any timeout, radio error, missing `TX_DONE`, or home
restore failure rejects that window. The final choice is the lowest-symbol
window with zero observed target-bit quiet hits, zero controlled misses, and
zero CAD timeouts, preserving the shortest bounded dwell.

A quiet result on an antenna exposed to an active RF environment is a
**non-fixture candidate event**, not proof of a false CAD positive: it may be
a real packet/preamble from another node. Therefore the strict false-positive
claim requires the fixture to be attenuated/coax-coupled or shielded during
the quiet controls. The harness can still collect an explicitly labelled
observe-only room-rate pilot without promoting it to that gate.

The first complete exposed-room pilot (three quiet and three pulsed Probes per
window) retained `COMPLETE`, `SD=1`, and home restore across all 30 cycles;
every capped-fixture pulse set the LongModerate bit. Its target-bit quiet-hit
counts were 0/3, 2/3, 2/3, 1/3, and 0/3 for 1, 2, 4, 8, and 16 symbols,
respectively. That measures the room, not CAD false positives. Sixteen symbols
also produced one CAD timeout in every one of its six cycles, so it is rejected
by the strict timeout rule despite its bit-only 3/3 pilot correlation. The
initial pilot JSONL predated that timeout field in its `accepted` summary; the
strict harness now records and rejects nonzero quiet or pulse CAD timeouts.
Artifact: `phase8-cad-rate-20260828T190616Z.*` under
`hardware-results/private/phase8/`.

Follow-up lower-gain testing retained a strong fixture link at four symbols
(20/20 capped pulses, zero CAD timeouts), but its 20 quiet controls still set
the LongModerate bit 14 times. Turning the local mesh off and placing both
devices in a metal ammo box did not make that antenna-attached control quiet:
the repeated four-symbol preflights were 2/3 target hits and 3/3 fixture
hits. In contrast, a retained ten-cycle, four-symbol quiet-only diagnostic
with the **Cardputer receive antenna disconnected** had zero LongModerate
target hits (while unrelated candidate activity remained). That isolates the
recurring target activity to the Cardputer antenna path, but does not identify
or quantify its outside source; the ammo box/USB arrangement is not a
calibrated shield. Production therefore remains at the upstream two-symbol
setting, and no measured false-positive rate or production CAD retune is
claimed. Artifacts: `phase8-cad-rate-20260828T200710Z.*`,
`phase8-cad-rate-20260828T201901Z.*`,
`phase8-cad-rate-20260828T202210Z.*`, and
`phase8-cad-rate-20260828T202539Z.*` under
`hardware-results/private/phase8/`.

### Initial candidate-specific bench correlation

Serial Control STATUS now publishes `C=free,detected,timeout,error` and `M`,
the latest Probe's candidate-index detection bitmask. With the Heltec V4 R8
configured as a capped -9 dBm LongModerate fixture, pre-arming it made the
LongModerate bit appear on two controlled runs, each with fixture `TX_DONE`,
`SD=1`, zero timeout/error, and Watch restored to 906.875 MHz. Those historical
runs used plan version 1, where LongModerate was bit `0x02`; version 2 inserts
the local MeshOregon tuple first, so LongModerate is now bit `0x04`. One quiet
control produced `M=00` and seven CAD-free observations. The second pulse
also saw ambient candidates (`M=4A`), so this is intended-tuple evidence, not
a claim of a noise-free environment or a measured false/miss rate. The earlier
arm-after-Probe attempt yielded `M=80`; it is retained as rejected timing
calibration, not counted as a LongModerate result.

The version-2 MeshOregon fixture passed four controlled pulses: the initial
smoke run plus the three-cycle pilot each reported `TX_DONE` and included the
first-candidate bit (`M=0021`, `0021`, `0109`, and `0005` respectively). Every
terminal record was `COMPLETE`, `SD=1`, `F=906875`, and advanced recovery once.
The matching three quiet controls left the MeshOregon bit clear (`M=0000`,
`0088`, and `0020`), but two contained activity on other candidates. Thus this
is four-for-four intended-tuple correlation and zero-of-three candidate-bit
quiet hits in an RF-active room, not an estimated false-positive or miss rate.
The attempted ten-cycle quiet process has no completion record because the
interactive host command exceeded its transport window; it is retained but
not counted.

### Packet-bearing Meshtastic interoperability gate

The Heltec V4 R8 was temporarily loaded with the official Meshtastic image
and configured to the fixed LONG_MODERATE candidate tuple. A persistent
Meshtastic serial API connection sent ten text packets while the Cardputer
ran one Probe. The Cardputer terminal reported `B=COMPLETE`, `SD=1`, `R=1`,
`C=6,2,0,0`, and `M=0024`; version 2 places LongModerate at candidate index 2
(`0x0004`), so the expected candidate bit was present. This is the first
application-protocol packet-bearing evidence for the candidate path; the
repository's deterministic Heltec bench image was restored after capture.
Artifact: `hardware-results/private/phase8/phase8-meshtastic-longmod-interop-20260828T2052.serial.log`.

## Decisions for this implementation slice

- Built-in plans are fixed, versioned, and compiled from source-backed data.
- Persistent custom candidate editing remains post-Phase-8 scope.
- Reticulum and General Exploration remain empty here; they are Phase 9
  energy-sweep consumers, not fixed discovery plans.
- The plan layer is pure and allocation-free. The first integration slice now
  adds radio-owned bounded CAD acquisition, a separate fixed observation queue,
  packet-bearing receive-on-hit, home restoration, and durable `probe.csv`.
  Transient mode remains post-Phase-8 work; deterministic cancellation/fault
  injection and the 1,000-cycle automated bench gate are complete.

## Open validation gates

- Expand the controlled quiet/pulse matrix before treating false-positive or
  miss rates as measured, and compare CAD symbol counts only against that
  matrix. Two symbols remain source-backed but are not yet this device's
  measured optimum.
- A shielded/attenuated quiet fixture is still needed to compare CAD symbol
  counts with a measured false-positive/miss matrix. The packet-bearing
  Meshtastic gate is complete; a legacy MeshCore node remains optional
  follow-up coverage rather than a Phase 8 exit condition.

## Primary sources

- Meshtastic Radio Settings: US slots and primary preset tuples:
  https://github.com/meshtastic/meshtastic/blob/master/docs/about/overview/radio-settings.mdx
- Meshtastic `RadioInterface.cpp`: standard preset list and US region:
  https://github.com/meshtastic/firmware/blob/develop/src/mesh/RadioInterface.cpp
- Meshtastic `RadioInterface.h`: two-symbol CAD setting:
  https://github.com/meshtastic/firmware/blob/develop/src/mesh/RadioInterface.h
- Meshtastic `RadioLibInterface.h`: current sync word `0x2B`:
  https://github.com/meshtastic/firmware/blob/master/src/mesh/RadioLibInterface.h
- MeshCore `MyMesh.h`: upstream companion default radio tuple:
  https://github.com/meshcore-dev/MeshCore/blob/main/examples/companion_radio/MyMesh.h
- MeshCore FAQ: current USA/Canada narrow recommendation:
  https://github.com/meshcore-dev/MeshCore/blob/main/docs/faq.md
- CascadiaMesh local setup guidance: USA/Canada preset and 910.525 MHz:
  https://cascadiamesh.org/getting-on-the-mesh/ and
  https://cascadiamesh.org/companion-devices-commercial/
- Semtech SX1262 product resources, including AN1200.48:
  https://www.semtech.com/products/wireless-rf/lora-connect/sx1262
- Semtech CAD FAQ: https://www.semtech.com/design-support/faq/faq-lora/P40
- RadioLib SX126x implementation/API:
  https://github.com/jgromes/RadioLib/blob/master/src/modules/SX126x/SX126x.cpp
  and https://github.com/jgromes/RadioLib/blob/master/src/modules/SX126x/SX126x.h
- RadioLib issue documenting the blocking-CAD hang risk:
  https://github.com/jgromes/RadioLib/issues/1830
