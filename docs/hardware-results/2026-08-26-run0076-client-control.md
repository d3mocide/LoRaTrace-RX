# Phase 7 client-control evidence — run0076 — 2026-08-26

This fresh run compared a no-client AP cycle with a client-only AP cycle.
The USB serial interface dropped during the latter cycle, so the display
reading is retained as operator evidence while the final serial lifecycle
checkpoint remains missing. The raw capture is in the ignored private file
`hardware-results/private/2026-08-26-phase7-p2-client-control-serial.log`.

## No-client control — run0072

- Fresh idle heap settled around 233,804–234,204 B before WiFi.
- AP was enabled for 60 seconds with the phone WiFi disabled; no client
  association was reported.
- Immediate stop checkpoint: 209,712 B free, 163,828 B largest block, 20/291
  blocks. The current heap then recovered to 216,744 B and remained there for
  the complete five-minute off-state settling window.
- `qdrop`, `busmiss`, and `rowdrop` remained 0.

## Client-only control — run0076

- Fresh idle heap settled around 233,952 B before WiFi.
- AP was enabled and the phone joined `LoRaTrace-7850` for 60 seconds without
  opening a browser or requesting a page. The serial log reports one client.
- While connected, current heap stabilized around 175,344 B; the observed
  low-water mark reached 107,768 B. No web request or CSV-transfer checkpoint
  was observed.
- After the phone disconnected and the AP was disabled, the USB serial link
  dropped before the `[wifi] AP stopped` and `wifi-stop-after` lines could be
  captured. The operator reported a stable 211 KB free heap on the device
  display after more than five minutes.

## Interpretation

The two fresh controls show a repeatable provisional difference of roughly
5–6 KB after a client has associated, even with no intentional HTTP workload.
This points to a retained client/TCP or delayed WiFi teardown allocation, but
the client-only largest-block/block-count checkpoint is missing and the serial
link failure prevents treating it as accepted P2 evidence. Repeat once with a
stable serial connection (or retrieve run0076's session CSV) before changing
WiFi lifecycle code or resizing stacks.

**P2: still open; client-associated cost is provisionally observed, not yet
proven persistent or isolated to a specific allocation.**
