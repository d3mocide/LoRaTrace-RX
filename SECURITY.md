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
  enables it from the on-device menu, the device starts a WPA2-PSK AP whose
  key is **generated per device** on first use from the hardware RNG (12
  characters, ~60 bits) and stored at `/loratrace/wifi.txt`. Read it on the
  device at **System > Connectivity > WiFi Key**; it is deliberately not
  available over serial, HTTP, or any export, and is not printed to the
  boot log. To rotate it, delete `/loratrace/wifi.txt` and re-enable the AP.
  If the card cannot be written the key still protects that session, but it
  changes on reboot and the device says so.
  Before 2026-09-07 this was a single hardcoded password shared by every
  build (`loratrace123`); **anyone running a build from before then should
  treat their AP as public.**
  Beyond the PSK there is still no separate login and no TLS: anyone who
  joins reaches the dashboard, settings endpoints, and CSV downloads over
  plain HTTP. State-changing endpoints now require a per-AP-session
  anti-CSRF token and pass an Origin check, so a web page the operator
  happens to be browsing cannot silently reconfigure the device — that is a
  defense against other sites, not against someone who has joined the AP.
  Don't enable it around people you don't want reading your run data.
- **SD card contents.** `detections.csv`/`session.csv`/run directories are
  plaintext CSV containing GPS coordinates and captured mesh metadata
  (node IDs, RSSI, etc. — see `detection.h`). Anyone with physical access
  to the SD card gets this in the clear. There is no on-device encryption
  of stored data.
- **Build and release supply chain.** As of 2026-09-07 every GitHub Action
  is pinned to a commit SHA rather than a mutable major tag, PlatformIO and
  every `lib_deps` entry are pinned to exact versions, workflows are
  read-only by default with write scoped to the one publishing job, the
  release tag is validated and passed through the environment rather than
  interpolated into shell text, the native tests run on the exact release
  ref, and every published flash part ships a SHA256SUMS and BUILDINFO file.
  The web flasher's CDN script remains an unvendored dependency.

- **Physical access.** This is a handheld embedded device with no secure
  boot / flash encryption configured. Physical possession of the hardware
  is assumed to grant full read/write access to firmware and stored data;
  this is out of scope for a security fix and treated as an accepted
  constraint of the platform, not a vulnerability.
- **Serial/USB Serial Control.** Diagnostic output is always available.
  The bounded command endpoint is disabled by default and only accepts its
  small allowlist after the operator enables System > Serial Control on-device.
  That physical enable is persisted in NVS so an ESP32-S3 native-USB host can
  reconnect after reset; explicit on-device disable or `LOW_PROFILE_OFF`
  turns it off. It can request existing Trace, Meshtastic/
  MeshCore profile, and Probe actions, but exposes no shell, files, arbitrary
  RF configuration or direct radio access. It also exposes Sweep, SD retry,
  and WiFi enable/disable commands; an enabled USB host can therefore expose
  the AP. The fixed AP password risk above applies to that path too. Treat an enabled
  USB endpoint as physical-presence authorization, not as a secret channel.
  Bluetooth LE control is not implemented or advertised yet; it must not be
  added without the authenticated-pairing and measured-memory gate documented
  in `research/phase8-low-profile-harness-design.md`.

## Passive decryption and operator keys

Public-channel decryption and decryption using known operator-supplied keys
are permitted by operator decision (2026-09-07). Brute-force, dictionary
attacks, key guessing, and automated key recovery are out of scope. The
firmware currently decrypts only Meshtastic public-default-key NodeInfo;
operator-key support and general payload decoding are future work.

Operator-key use must require explicit configuration and an on-device opt-in,
disabled by default. Its implementation must define key import, storage,
removal, and access controls within this device's physical-access limits.
Keys must not appear in source control, serial diagnostics, capture CSVs,
browser responses, or shareable exports. Preserve raw packets independently
of derived plaintext. Treat decoded content as sensitive even on public
channels, and never equate decryption or an advertised public key with an
authenticated identity. MeshCore advert signatures are not verified today.

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
