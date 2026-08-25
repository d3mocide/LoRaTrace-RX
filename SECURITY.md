# Security Policy

LoRaTrace RX is RX-only wardriving firmware for a single-owner handheld
device (Cardputer-Adv + SX1262). It has no transmit path, no cloud
service, and no multi-tenant surface — the threat model here is narrow
compared to a typical networked application, but it's not zero: the
device captures other people's mesh traffic (including position and node
metadata) to local storage, and can optionally expose a WiFi AP + web UI
in the field.

## Supported versions

Only the latest tagged release (`vMAJOR.MINOR.PATCH`, see
[CHANGELOG.md](CHANGELOG.md) and `src/version.h`) is supported. This is a
hobbyist/field-research firmware project without a maintenance branch
model — fixes land on `main` and ship in the next tag, they are not
backported.

## Known attack surface

- **WiFi AP + web UI (`wifi_task.cpp`), off by default.** When an operator
  enables it from the on-device menu, the device starts a WPA2-PSK AP with
  a **fixed default password** (`loratrace123`, hardcoded in
  `wifi_task.cpp`). Anyone who has or guesses that password and is in RF
  range can join the AP and reach the dashboard, settings endpoints, and
  CSV downloads over plain HTTP — there is no separate login on top of the
  WiFi PSK, and no TLS. Treat the AP as reachable by anyone within range
  who knows (or brute-forces) the shared password; don't enable it around
  people you don't want reading your run data or changing your channel
  settings.
- **SD card contents.** `detections.csv`/`session.csv`/run directories are
  plaintext CSV containing GPS coordinates and captured mesh metadata
  (node IDs, RSSI, etc. — see `detection.h`). Anyone with physical access
  to the SD card gets this in the clear. There is no on-device encryption
  of stored data.
- **Physical access.** This is a handheld embedded device with no secure
  boot / flash encryption configured. Physical possession of the hardware
  is assumed to grant full read/write access to firmware and stored data;
  this is out of scope for a security fix and treated as an accepted
  constraint of the platform, not a vulnerability.
- **Serial/USB console.** Diagnostic output only (see CLAUDE.md's Serial
  mutex notes) — there is no command console or remote-control path over
  serial today. If one is ever added, it needs its own threat-model pass
  before shipping (this was explicitly deferred once already, see
  CHANGELOG.md).

## Data sensitivity

Wardriving output (`detections.csv`, run directories) records other
people's mesh-network traffic and GPS-correlated positions. Handle
exported logs the way you'd handle any other RF/location capture data —
this firmware makes no attempt to anonymize or restrict what it records,
that responsibility sits with the operator.

## Reporting a vulnerability

This is a personal/open-source hardware project, not a maintained product
with an SLA. To report a security issue:

- Open a [GitHub issue](https://github.com/d3mocide/LoRaTrace-RX/issues)
  for anything that isn't sensitive to disclose publicly (e.g. the fixed
  WiFi password design tradeoff above is already public and documented).
- For anything you believe needs coordinated/private disclosure (e.g. a
  bug that goes beyond the known limitations above — RCE from the web UI,
  a way to read/write SD contents without the WiFi PSK, etc.), use
  GitHub's private vulnerability reporting on this repository
  (Security tab → "Report a vulnerability") rather than a public issue.

There's no bug bounty; this is best-effort, maintained in whatever time
the project gets.

## Out of scope

- Attacks requiring physical possession of the device or SD card.
- The WiFi AP's fixed default password being guessable/shared — this is a
  documented, deliberate tradeoff (see `wifi_task.cpp`'s own comment), not
  an oversight. Making it SD-configurable is a tracked follow-up
  improvement, not a vulnerability report.
- Radio-layer attacks against the mesh protocols themselves (Meshtastic,
  MeshCore, Reticulum) — this firmware is a passive RX-only listener and
  doesn't implement or defend those protocols.
