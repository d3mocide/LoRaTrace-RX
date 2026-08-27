# Phase 7 — run0095 — 15-minute combined-load control

Date: 2026-08-26 local / 2026-08-27 UTC  
Firmware: v0.6.8 (`a24abaa-dirty`)  
Artifact: `.pio/build/cardputer-adv/firmware.bin` (SHA-256
`ad17b3eb069845ce97811a084bcaaea90899e4efd15b9d3322c45b2be96ca9e9`)  
Capture: `hardware-results/private/2026-08-27-phase7-serial-flush-final-v3.log`

## Workload

- Fresh boot, GPS fix, SD logging, UI, and radio active.
- Several AP start/stop cycles, then one phone client associated with the
  AP and the dashboard left active.
- Browser polling, run-list/CSV activity, and profile/radio settings saves
  were exercised while the device received live RF traffic.
- Capture was held for approximately 15 minutes (the operator stopped early
  for time; the 30-minute/800-detection Baseline E target remains open).

## Observed result

Final status sample (run 95):

```
rx=127  rows=127  crcerr=0  qdrop=0  busmiss=1  rowdrop=0
heap=170184  heapmin=105736  nmea=16273  badcrc=1
```

The single `busmiss=1` occurred while the operator was saving radio settings
through the active WiFi path and did not increase afterward. `badcrc=1` is a
GPS-sentence checksum count, not a radio CRC error. Current heap returned to a
flat ~170 KB between browser requests; AP start/stop snapshots retained a
167,924-byte largest block.

The enlarged native USB-CDC TX ring, connection guard, and complete-write
helper made the GPS clock line and AP start/stop announcements complete in
this run. Long diagnostic lines (especially `wifi-*-before` and CSV/config
messages) still occasionally arrived with a clipped prefix under WebServer
activity, so serial text remains supplemental to `session.csv`/status
counters and the transport issue stays a Phase 7 watch item.

## Decision

**Baseline E closed by operator acceptance (early stop).** No heap trend,
queue drops, or row drops appeared during the 15-minute window. The operator
accepted this as sufficient combined-load evidence rather than extending to
the matrix's nominal 30-minute/800-detection target. The one bus miss is
attributed to the concurrent settings save, not a sustained contention trend.
