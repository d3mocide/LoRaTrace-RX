# CSV download watchdog fix — 2026-08-27

## Failure captured

- Firmware: v0.6.8, build revision `d3c4fe4-dirty`
- Reproduction run: 105
- Trigger: Downloads-tab request for `run0102/detections.csv`
- Before the failure: `[mem] csv-download-before`, followed by the expected
  empty-body warning from the WebServer header-only response.
- Failure: CPU 0 WiFi task held the core in `WiFiClient::write()` until the
  task watchdog fired at about 64.7s. The board aborted and rebooted as run
  106.
- Symbolized backtrace terminates at `streamCsvFile()`'s client write
  (`src/wifi_task.cpp:193`), called from `handleNotFound()`.

## Fix and validation

- Changed `streamCsvFile()` to copy the client handle, reject a disconnected
  client, stop on a short write, and yield for 1ms after every successful CSV
  chunk.
- Native tests: 91/91 passed.
- Cardputer-Adv build: successful; 50,348B static RAM and 972,661B flash.
- Flashed firmware SHA-256:
  `f49b16ad88fadec6b6543ad21bd4888da16bbba5135e0f2f44c4bf663f41a58d`
- Validation run: 108, same operator-triggered `run0102/detections.csv`
  download completed successfully.
- After the transfer the client disconnected normally; no watchdog, panic,
  backtrace, or reboot occurred. The post-fix capture continued reporting
  normal status rows.

The `content length is zero` warning remains a benign consequence of using
WebServer's header-only response setup before manually streaming the body; it
was not the crash cause.
