# Phase 7 client-control evidence — run0081 — 2026-08-27

This repeat used the raw serial capture after the USB cable was secured. The
full reset/boot banner was captured: firmware `v0.6.8`, build revision
`a24abaa`; logger run `81`. The raw capture is retained in the ignored private
file `hardware-results/private/2026-08-27-phase7-p2-client-control-retry2-serial.log`.

## Client-only control

- Fresh WiFi-off heap settled at 233,952 B.
- AP start checkpoint: 178,124 B free, 167,924 B largest block, 4/352 blocks.
- The phone associated, briefly flapped once because the AP has no Internet,
  then remained connected for a clean 60-second hold. No browser or deliberate
  HTTP request was made.
- During the stable hold, current heap was about 175,356 B and the low-water
  mark reached 98,516 B. `qdrop` and `busmiss` stayed 0.
- AP stop checkpoint: 209,592 B free, 167,924 B largest block, 17/292 blocks.
- Within the next status sample the heap recovered to 216,624 B. It remained
  exactly 216,624 B for the full five-minute off-state settling window; the
  no-client run0072 control settled at 216,744 B.
- One `crcerr` appeared late in the idle tail; `qdrop`, `busmiss`, and
  `rowdrop` remained 0. The CRC error is unrelated to the WiFi lifecycle
  allocation comparison but means this run is not a zero-error RF soak.
- Several individual serial lines were malformed during the AP/client burst
  (`wifi-start-before`, the AP-start announcement, and one client event), even
  though the lifecycle memory lines and later status lines were complete. The
  serial text is therefore not sufficient evidence for missing events until
  the USB-CDC transport issue is mitigated.

## Interpretation

The client-associated heap cost was fully reclaimed after AP shutdown. The
settled free-heap difference from the no-client control was 120 B, with no
persistent largest-block loss. The earlier ~5–6 KB display deficit was a
short-window observation during delayed heap coalescing, not evidence of a
client leak or a request-state retention bug.

**P2 lifecycle/request optimization: no persistent client cost confirmed;
leave WiFi teardown code unchanged.** Baseline D (browser/download/settings
workload) remains open separately, and serial transport truncation remains a
diagnostic watch item.
