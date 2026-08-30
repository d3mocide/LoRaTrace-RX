# LoRaTrace RX — Hardware Validation

This is the repeatable real-device test protocol. `docs/STATUS.md` and
`CHANGELOG.md` record what passed; this file defines how to test it.
Phase 7 uses it to establish
memory headroom before discovery and energy scanning add new radio states.

## Evidence to keep

Every tested build must be identified by the boot banner's semantic version
and build revision. Preserve:

- the full serial capture from boot through shutdown;
- the run's `session.csv` before interpreting `detections.csv`;
- the workload and approximate timing of every WiFi toggle/download;
- the firmware build's reported static RAM and flash use;
- any visible UI regression, ideally with a photo.

Store tracked, location-redacted summaries under `hardware-results/`.
Raw serial/CSV evidence belongs under `hardware-results/private/`, which is
git-ignored because those files can contain precise GPS locations. Record an
artifact SHA-256 whenever testing a dirty-tree build: the injected short Git
revision alone cannot distinguish two different `-dirty` binaries.

Do not treat `heap_min` alone as a leak signal. It can only fall. Compare
current `heap_free`, `heap_largest`, and block counts before and after a
workload to tell a recovered transient allocation from fragmentation.

## Phase 7 memory fields

`session.csv` appends these fields after the pre-Phase-7 schema:

- `heap_largest` — largest currently allocatable internal 8-bit block;
- `heap_free_blocks` / `heap_allocated_blocks` — fragmentation context;
- `radio_stack_free`, `gps_stack_free`, `ui_stack_free`, and
  `wifi_stack_free` — lifetime minimum unused stack bytes for the four tasks
  Phase 7 adds; the existing `logger_stack_free` column remains in its
  original position and is sampled through the same mechanism.

The existing `heap_free`/`heap_min` columns keep their original
`ESP.getFreeHeap()`/`ESP.getMinFreeHeap()` meaning for comparison with old
runs. The newly appended heap fields specifically describe internal 8-bit
allocatable memory.

A stack value of 0 means the task had not registered when that row was
written. Periodic rows after startup should contain all five values.

Serial `[mem]` checkpoints bracket the canvas allocation, WiFi start/stop,
and CSV downloads. They are lifecycle evidence, not per-packet logging.

## Phase 7 baseline matrix

Run the matrix in order on the same build and SD card. A failed stage stops
the matrix until its cause is understood.

### A. Boot and idle

1. Cold boot with WiFi off.
2. Confirm the indexed canvas is active and the UI is free of tearing.
3. Leave radio, GPS, logger, and UI running for 10 minutes.
4. Record the stable heap fields and all five stack watermarks.

### B. Normal receive and logging

1. Supply real Meshtastic or MeshCore traffic.
2. Accumulate at least 100 detections and several SD flushes.
3. Exercise carousel pages, all menu levels, brightness, idle dim, Trace
   pause/resume, and a profile switch while paused.
4. Confirm `queue_drop`, `row_drop`, and `bus_miss` remain 0.

For the manual UI pass, record these exact transitions in the run notes:

1. Carousel: visit pages 1, 2, 3, and 4, then wrap once with PREV/NEXT.
2. Open the menu with ESC; select Trace and confirm RADIO shows `STANDBY`.
3. While paused, enter Profile and select the other profile; confirm the
   CHANNEL page changes. Profile switching resumes the radio by design, so
   enter Trace again, pause it, then enter Trace once more and confirm RX
   continues after the explicit resume.
4. Enter System > Display > Brightness, step the slider both directions,
   then back out; cycle Idle dim and wait through its selected timeout.
5. Return to the carousel. Confirm every frame is tear-free and no menu,
   toast, slider, or dim transition clips or crashes.

After copying the run's `session.csv` from the card, the objective portion can
be checked without hand-counting rows:

```text
python3 scripts/check_phase7_baseline.py --session session.csv --ui-ok
```

The command intentionally keeps the UI confirmation manual: a CSV can prove
the counters and stack telemetry, but cannot prove what the real glass showed.

### C. WiFi lifecycle

1. Toggle the AP on and wait 30 seconds.
2. Toggle it off and wait 30 seconds.
3. Repeat for 10 complete cycles without connecting a client.
4. Compare each `wifi-start-*` / `wifi-stop-*` checkpoint. Current free
   heap and the largest block must settle rather than trend downward.

### D. Browser workload

With the AP active and a real client connected:

1. Poll the status dashboard for 5 minutes.
2. Open the `Downloads` tab and repeatedly retrieve the available run files;
   the web firmware has no separate operator-facing run-list control.
3. Download a realistically large `detections.csv` five times.
4. Download `session.csv` five times.
5. Save each profile's settings, then reboot and confirm they apply.

Use the `[mem] csv-download-*` pairs to identify any transient trough and
whether current heap/largest-block values recover after each transfer.

### E. Combined load

Keep WiFi and one browser client active while GPS has a fix and the radio
logs real traffic. Target at least 800 detections or 30 minutes, whichever
takes longer. Exercise downloads during reception. The four data-path
counters (`crc_err`, `queue_drop`, `bus_miss`, `row_drop`) must not worsen
because WiFi is active.

### F. Soak

Only after A-E pass, run for at least two hours with the intended field
configuration. A flat post-warm-up trend in current free heap and largest
block is the no-leak result; a falling `heap_min` by itself is not.

Use the final stable measurements to record an explicit accept/reject decision
for a fixed 2.5 KB transient Probe/Sweep result buffer. Acceptance does not
permit a second result copy, raw-sample history, or dynamic growth; rejection
makes SD mandatory for those modes. Preserve the decision and evidence in
`CHANGELOG.md`, alongside the other Phase 8/9 budgets recorded in
`docs/history/PROGRESS.md`.

## Optimization acceptance rules

- Change one memory lever at a time and repeat the affected matrix stages.
- Never reduce a task stack until its worst workload has been measured.
- After reduction, retain at least 25% and 1KB of observed stack headroom;
  use the larger requirement.
- Do not trade nonzero drop/error counters for a better heap number.
- Do not replace the 32KB indexed canvas unless measurements show it is the
  limiting allocation; any replacement must repeat the full UI/glass pass.
- Moving a buffer from heap to static RAM is not a memory saving. Count
  total SRAM and runtime behavior, not only the displayed free-heap number.
- Phase 7 is complete only when the final combined-load and soak results are
  recorded in `CHANGELOG.md` with the tested build revision.

## Device connection

For the fastest instrumented loop, connect the Cardputer-Adv over USB and
use the direct PlatformIO environment:

```text
pio run -e cardputer-adv --target upload
pio device monitor
```

Opening the monitor can reset the board through DTR, which creates a new run
directory. Start the retained serial capture at that reset and use the new
run number. Launcher SD-drop remains valid for release-like verification,
but direct USB flashing is preferred while Phase 7 is iterating.
