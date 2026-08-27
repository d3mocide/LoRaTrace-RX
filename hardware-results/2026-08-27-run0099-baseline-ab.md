# Phase 7 hardware evidence — run0099 — 2026-08-27

## Build and artifacts

- Firmware: v0.6.8, build revision `d3c4fe4-dirty`
- Firmware SHA-256: `f55a13f811ac70b5963d430e3f092cbec4f079ec3741c464352e1ea0c9a48ae3`
- Static RAM: 50,348B; flash: 972,637B
- Profile: MeshCore, `910.525 MHz / SF7 / BW62.5 kHz / CR4/5 / sync 0x12`
- Downloaded `session.csv` SHA-256: `0fdade4433fc84d89774f624ac0c63e60a8cf2d18993a9ae81cb4e810f09b6bd`
- Downloaded `detections.csv` SHA-256: `ef235e1f5bca2c6a6f150f34443fef3d3ae62f0bdda122977d8288af4c754ca8`

## Baseline A — boot and idle: PASS

Run 99 stayed up for 1,089 seconds with WiFi off through the 10-minute
mark. From the first settled periodic row at 66s through 789s, free heap was
233,848B, largest block was 221,172B, and the block counts stayed 7/165.
The indexed canvas was active, and the operator observed no tearing, flicker,
clipping, or crash during the subsequent UI pass.

Post-registration stack watermarks were:

| Task | Minimum unused stack |
|---|---:|
| Radio | 2,132B |
| GPS | 1,444B |
| Logger | 1,800B |
| UI | 2,020B |
| WiFi | 3,220B |

## Baseline B — receive and logging: PASS

- 191 received and 191 logged detections
- 157 detection flushes
- `crc_err=0`, `queue_drop=0`, `bus_miss=0`, `row_drop=0`
- `nmea_bad_crc=0`
- Worst detection flush: 61ms before the later WiFi retrieval step
- The operator exercised carousel paging, menu nesting, Trace pause/resume,
  profile switching while paused, brightness, idle dim, and the wake path.
  The display was reported as beautiful with no visible regression.

The AP was enabled after the A/B interaction pass for the browser workload
and to download the two CSV files. The shipped web firmware exposes those
files through the `Downloads` tab; there is no separate operator-facing
run-list control. The final 1,089s session row therefore reflects WiFi-on
allocation (169,300B free heap, 155,636B largest block); it is excluded from
the WiFi-off idle interpretation above.

The operator also confirmed completion of the Baseline D dashboard polling,
repeated CSV downloads, and both profile settings saves on this build. The
serial capture was closed before that AP session, so its transient
`csv-download-*` checkpoints are not part of the retained serial artifact;
the downloaded files and final session row remain the run's evidence.

## Decision

**Baseline A/B and D accepted for run 99.** The raw serial capture remains in the
git-ignored `hardware-results/private/` directory; the downloaded CSVs retain
the precise GPS data locally and are not checked in.
