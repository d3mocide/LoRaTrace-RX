# LoRaTrace RX — Progress Tracking

Living document. Update this alongside code changes — it's the "what's
actually true right now" source, `ROADMAP.md` is the "what's the plan"
source, `DESIGN.md` is the "why" source. Don't let them drift.

## Current status (2026-08-22)

Phase 0 (project scaffold) complete. Phase 1 (RadioLib bring-up, now
including optional SD-based channel config) **boots successfully on real
hardware** (SD-dropped via Launcher, not direct USB flash): antenna-switch
IO-expander init, SX1262 init over FSPI, and `radio.startReceive()` all
succeed, landing on the hardcoded Meshtastic LongFast (US) default
(906.875MHz/SF11/BW250/CR4:8), and the microSD card mounts successfully on
the shared bus. See the first 2026-08-22 Decisions log entry for the full
read of that boot log, including why some of its noisier lines (a
reset-reason banner and a core-dump-partition warning) are not evidence of
a firmware crash. **Still unverified on hardware:** actual receipt of a
live Meshtastic packet (no `[RX]` line yet) and the SD-based config
override path.

Same-day follow-up work, **confirmed to build cleanly** (`pio run -e
cardputer-adv`, `esp32-s3-devkitc-1`: RadioLib 7.7.1 + GFX Library for
Arduino 1.4.0, 406KB flash / 20.6KB static RAM) and **now also flashed to
hardware** two more ways, same day: a direct USB flash (`pio run --target
upload`, first time this repo's own board def/partition table has run
outside a Launcher sideload — user confirmed it booted with serial output),
and a second Launcher SD-drop, whose full serial capture surfaced two new
findings — see checklist/Decisions log:
- The boot-status splash renders and is legible, but doesn't fully clear
  the physical panel: returning from Launcher's own UI leaves visible
  remnants of Launcher's screen behind LoRaTrace's splash ("kind of
  glitched," per user report) — likely the untested IPS offset/rotation
  constants in `board_pins.h`, not yet root-caused.
- That same boot's SD mount **failed** (`sdCommand(): crc error` /
  `GO_IDLE_STATE failed`), so the SD-config override still couldn't be
  exercised — falls back safely to the hardcoded default exactly as
  designed, and the user's edited `config.txt` was left untouched on the
  card (mount failure happens before the file is ever opened). A
  hypothesis fix is now applied in `main.cpp` (untested — see Decisions
  log) targeting a GPIO5/`PIN_LORA_NSS` timing note already on record in
  `board_pins.h`.
- `/loratrace/config.txt` is now auto-created (pre-filled with the current
  defaults) on the first card this firmware sees, instead of requiring an
  operator to hand-copy `sd-template/loratrace/` themselves.
- A boot-status splash now draws to the ST7789 LCD, mirroring the serial
  banner (including FATAL messages) — a narrow, one-shot exception to
  CLAUDE.md's Phase 6 UI gate, not the `ui_task`. See Decisions log for
  why, and for the researched fix to the separate "can't get back to
  Launcher" problem this was raised alongside.

CI includes a rolling `dev-latest` release for grabbing a
Launcher-installable `.bin` without tagging — see Decisions log.

## Build-order checklist

Mirrors `ROADMAP.md` phases / `DESIGN.md` §9.

- [x] **Phase 0** — platformio.ini, pin map, RF tables, phase-1 code, docs
- [ ] **Phase 1** — RadioLib bring-up (builds clean, boots on hardware,
      packet RX + SD-config override still unverified)
  - [x] Confirm `pio run` builds successfully (312KB flash / 20.3KB RAM,
        `esp32-s3-devkitc-1`, RadioLib 7.7.1)
  - [x] Flash to real Cardputer-Adv + Cap LoRa-1262 and confirm it runs
        (2026-08-22, via Launcher SD-drop of `LoRaTraceRX-dev.bin` — see
        Decisions log for the boot log read)
  - [x] Confirm `esp32-s3-devkitc-1` board def actually matches this
        board's flash/pin config (no dedicated PlatformIO board def found)
        — the first 2026-08-22 boot was a Launcher sideload, which uses
        Launcher's own partition table, not this repo's own
        `platformio.ini`/`default_8MB.csv` (see ROADMAP.md Distribution
        section). 2026-08-22 (later same day): direct USB flash (`pio run
        --target upload`) succeeded — `esptool` identified the chip
        correctly (ESP32-S3 QFN56 rev v0.2, 8MB flash) and wrote
        bootloader + partition table + app cleanly at offsets
        0x0/0x8000/0x10000, meaning this repo's own board def/partition
        table is now actually running on the device, not Launcher's. User
        confirmed it booted with serial output; the specific log from that
        boot wasn't captured/pasted in this session (a later Launcher
        SD-drop boot was captured instead — see Decisions log), so this is
        closed on a lighter-weight user report than the fully-logged
        boots, not a captured transcript
  - [x] Confirm PI4IOE5V6408 I2C address (0x43) and register map
        (0x03/0x05/0x07) against real hardware — sourced from a
        third-party driver, not the primary Diodes datasheet directly.
        2026-08-22: init completed without hitting the "FATAL: IO expander
        init failed" hang, i.e. all three I2C writes ACKed at 0x43. Confirms
        the *I2C register-write* path; still doesn't confirm the antenna
        path is RF-correct (that needs an actual received packet)
  - [x] Confirm FSPI is actually free (not claimed by the display bus) —
        2026-08-22: `radio.begin()` returned `RADIOLIB_ERR_NONE`, which
        requires working SPI read/write to the SX1262
  - [x] Confirm RadioLib 7.7.x `SX1262::begin()` / `setDio1Action()`
        signatures match what's in `main.cpp` (pinned by `^7.7.1`, minor
        version drift possible) — 2026-08-22: confirmed at runtime too
        (`begin()` and `startReceive()` both returned `RADIOLIB_ERR_NONE`
        on real hardware), not just at compile time
  - [ ] Bench-verify RX against a known live Meshtastic LongFast (US)
        transmitter — RSSI/SNR should be plausible, not just non-crashing.
        2026-08-22: device reached "Listening..." but no `[RX]` line was
        seen in that session — still open. Same day: a received packet
        now also updates a reserved splash line in place (`main.cpp`
        `updateRxSplash()`), so this can be confirmed from the screen
        alone — build-clean, untested on hardware like the rest of
        tonight's additions
  - [ ] Confirm SD-based channel config actually loads: drop
        `sd-template/loratrace/` onto the SD card with real values (e.g.
        MeshOregon's), confirm the boot banner reports the overridden
        channel, not the hardcoded default. 2026-08-22: card wasn't
        carrying `config.txt` yet, only the "not found, using built-in
        default" fallback had been exercised. 2026-08-22 (later same day):
        user edited `config.txt` to real override values and reflashed,
        but the override still didn't apply — root cause was the SD mount
        itself failing that boot (`GO_IDLE_STATE failed`, see checklist
        item below and Decisions log), not the config-parsing logic; the
        edited file was left untouched on the card (fails safe as
        designed). Still open, now blocked on the SD-mount reliability
        item below
  - [x] Confirm PIN_SD_CS (12) + shared SCK40/MISO39/MOSI14 actually mount
        the SD card — now sourced directly from Cardputer-Adv + Cap
        LoRa-1262's own official docs/pin diagram (see DESIGN.md §7).
        2026-08-22: confirmed on real hardware — see Decisions log.
        **Caveat added 2026-08-22 (later same day):** a second Launcher
        SD-drop boot failed to mount the same card at all (`sdCommand():
        crc error` repeated, then `GO_IDLE_STATE failed` /
        `f_mount failed: (3)`) — so mounting is proven possible but not yet
        reliable. Leading hypothesis: the GPIO5/`PIN_LORA_NSS` early-init
        timing note already on record in `board_pins.h` (from
        bmorcelli/Launcher's own Cardputer-ADV notes) — a fix driving that
        pin high before any I2C/SPI access is now applied in `main.cpp`,
        untested. Needs a reflash + repeat boot to know if it actually
        closes this out
  - [ ] Bench-verify auto-created `/loratrace/config.txt`: confirm it
        actually appears on a blank card after first boot, pre-filled with
        the current defaults, and that editing it and rebooting overrides
        the active channel — added 2026-08-22, build-clean. 2026-08-22
        (later same day): auto-create confirmed via Launcher SD-drop (user
        report: "the SDCard config built") — the file-creation half of
        this item is done. Still open: editing that file to a real
        override and rebooting to confirm the active channel actually
        changes (Next steps #3)
  - [x] Bench-verify the boot-status splash actually renders on the real
        ST7789 panel — pins/IPS offsets/rotation are sourced from
        bmorcelli/Launcher's confirmed-working Cardputer-ADV config, not
        independently bench-verified here (board_pins.h). Added 2026-08-22,
        build-clean. 2026-08-22 (later same day): confirmed via Launcher
        SD-drop (user report: "the GUI came up") — panel renders and is
        legible, not blank/garbled. Heartbeat-dot blink specifically not
        separately confirmed by that report. **Caveat, same day, second
        Launcher SD-drop test:** renders, but doesn't clear the *whole*
        physical panel — remnants of Launcher's own screen stay visible
        behind/around LoRaTrace's splash ("kind of glitched," per user
        report). See new checklist item below — closing this one on
        "renders and is legible," tracking the incomplete clear
        separately since it's a distinct, more specific defect
  - [ ] Fix/root-cause the boot-status splash not clearing the full
        physical panel (found 2026-08-22, second Launcher SD-drop test —
        see above and Decisions log). `initDisplay()` does call
        `tft->fillScreen(SPLASH_BG)` right after `tft->begin()`, so the
        leading hypothesis is that `TFT_PANEL_WIDTH`/`TFT_PANEL_HEIGHT` or
        the landscape IPS column/row offsets in `board_pins.h` (already
        flagged TODO(verify), sourced from Launcher's config rather than
        independently bench-verified) don't actually cover this panel's
        full visible window at rotation 1, leaving a border `fillScreen`
        never touches. Not yet root-caused or fixed — needs either a photo
        of the glitch (which edge/how much is left uncleared tells us
        whether it's an offset or a width/height mismatch) or bench
        experimentation
- [ ] **Phase 2** — task/queue architecture, GPS, SD, Logger (MVP-Beta)
- [ ] **Phase 3** — MeshCore profile
- [ ] **Phase 4** — `DISCOVERY_SWEEP`
- [ ] **Phase 5** — `ENERGY_SWEEP`
- [ ] **Phase 6** — UI polish, WiFi upload go/no-go

## Open questions (from DESIGN.md §7 — verify before / during build)

- [ ] Meshtastic's exact SX126x sync-word register value (sources
      disagree — pull from Meshtastic firmware source or RadioLib's
      Meshtastic-compat example)
- [ ] MeshCore's encryption/PSK scheme (don't assume it mirrors
      Meshtastic's default-channel PSK model)
- [x] microSD bus on this board revision — SPI, confirmed shared with the
      radio (not the display). **Finding (2026-08-12), well-sourced:**
      cross-checked M5Stack's official docs for both halves directly
      (Cardputer-Adv's own microSD pin table + the Cap LoRa-1262's own SPI
      pin table and printed pin-diagram image) — identical SCK/MOSI/MISO,
      distinct CS, and the Cardputer-Adv docs state outright that microSD
      shares pins with the EXT/Cap expansion connector. An initial hastier
      fetch of these same pages produced a scrambled, self-contradictory
      table (worth remembering as a caution about trusting single-pass doc
      scraping for anything pin-critical) — corrected via a stricter
      verbatim-quote re-fetch and a photo of the module's own diagram. See
      DESIGN.md §7 for the full note. Still not confirmed with an actual
      continuity/multimeter check on this board — that's the only thing
      left to close this out completely.
- [ ] Whether SD and the radio sharing one physical SPI bus needs explicit
      arbitration (e.g. a mutex around the shared `SPIClass`) once Phase 2
      makes both active concurrently across Core 0/Core 1 — the FreeRTOS
      queue moves the radio *task's* code off SD, but doesn't remove the
      electrical fact that two devices on one bus can't transact at the
      same instant. Not a Phase 1 problem (config load and radio.begin()
      happen sequentially in setup(), nothing concurrent yet) — flagging
      now so Phase 2's radio_task/logger_task design accounts for it.
- [ ] CAD `symNum` tuning — needs bench testing against Semtech AN1200.48
- [ ] 868–923MHz front-end rolloff near 923MHz — empirical RSSI floor
      sweep once hardware is in hand
- [ ] Exact per-slot frequency spacing for Meshtastic's 104 US slots —
      pull the real table from firmware source, don't infer from BW alone
- [ ] Real `ESP.getFreeHeap()` under load — every "no PSRAM" risk call in
      ROADMAP.md is provisional until this number exists. 2026-08-22: a
      boot-time snapshot now prints to serial and the splash
      (`main.cpp`, right after "Listening...") — still just a baseline at
      idle, not "under load," and still needs real hardware to produce an
      actual number. Closing this out fully needs a reading while the
      radio's actively receiving, once that's testable too.

## Open questions — Launcher distribution

- [x] Exact button combo for the manual-restart-into-Launcher path.
      **Resolved (2026-08-22), read directly from bmorcelli/Launcher's own
      source** (`src/main.cpp` `setup()`, not a guess or an issue-thread
      summary): on every reset, Launcher itself boots first and waits
      `2000 + bootToApp*3000` ms (bootToApp defaults `true`, i.e. **5
      seconds**) while polling for a keypress, printing "Press the button
      to enter the Launcher!" over serial every ~500ms during that window.
      Press *any* key (Sel/Enter goes straight to the menu; a digit
      1-9/0 boots that numbered slot directly; anything else opens the
      app-picker) before the window closes and you land in Launcher's
      menu; do nothing and `launcherBootCurrentApp()` chain-boots straight
      back into whatever ran last (LoRaTrace RX) — which reads as "stuck,
      can't get back to Launcher without reflashing" if that ~5s window is
      missed. **Durable fix, no firmware change needed on our side:**
      Launcher's own Settings menu has a "Boot to Launcher" toggle
      (`src/settings.cpp`) — enabling it sets `bootToApp = false`, and
      `launcherBootCurrentApp()`/`launcherBootInstalledAppOrShowMenu()`
      both hard-return `false` whenever `!bootToApp`, so Launcher always
      stops at its own menu on reset instead of auto-chaining an app,
      until the toggle is flipped back off. Confirms the 2026-08-12
      decision below ("not something our firmware needs to implement")
      was architecturally right — the missing piece was purely this
      operational detail, now printed on our own boot banner too (see
      Decisions log and `main.cpp`).
      **Contradicting report, 2026-08-22 (later same day):** user says
      interrupting boot shows Launcher's screen briefly, but the firmware
      keeps booting anyway rather than staying in Launcher's menu. Not yet
      root-caused — open questions include whether the key was held
      through the reset vs. tapped once (the polling-interval finding
      below says holding is what reliably works), whether the "Boot to
      Launcher" toggle was tried, and whether the display-glitch bug above
      makes a partially-drawn Launcher screen look like it "showed" when
      the keypress window had actually already closed. Needs more detail
      before treating this as a firmware or Launcher bug rather than
      timing/process — this finding was research against Launcher's own
      source, not invalidated by one report, but also not proven against
      real behavior yet either
- [ ] How much flash Launcher's own footprint (bootloader + its app
      partition + any data/SPIFFS it keeps) leaves free for user-installed
      apps on an 8MB device, especially with multiple firmwares installed
      side by side. No documented number found — see ROADMAP.md
      Distribution section for the size budget reasoning used in the
      meantime.

## Decisions log

- **2026-08-12** — Board id: used `esp32-s3-devkitc-1` in `platformio.ini`.
  No dedicated PlatformIO board definition for Cardputer-Adv was found;
  this is what's used in practice by the M5Stack/ESP32-S3 community for
  this chip. Revisit if M5Stack ships an official board file.
- **2026-08-12** — Deferred creating `radio_task`/`gps_task`/
  `logger_task`/`ui_task`/`fingerprint.h` (CLAUDE.md's proposed layout)
  until Phase 2. CLAUDE.md's Status section explicitly says start with
  phase 1 bring-up "before touching the task/queue architecture" — the
  scaffold respects that rather than creating empty stub files now.
- **2026-08-12** — Added `src/board_pins.h`, which isn't in CLAUDE.md's
  proposed layout. Justification: DESIGN.md §1 already tabulates the full
  pin map as decided config, multiple future tasks (radio, GPS, logger)
  will need the same constants, and Phase 1 needs them immediately — a
  shared header avoids duplicating pin numbers across files as they're
  added in later phases.
- **2026-08-12** — RadioLib pinned to `^7.7.1` (latest release as of this
  session). PI4IOE5V6408 I2C address (0x43) and register offsets
  (0x03 direction / 0x05 output / 0x07 high-Z) sourced from ESPHome's
  component docs and a from-datasheet MicroPython driver, not the primary
  Diodes datasheet — flagged for hardware verification, not treated as
  ground truth.
- **2026-08-12** — Confirmed (user): returning from a Launcher-installed
  app to Launcher itself is a manual restart + button combo, not a
  software hook our firmware needs to implement. No return-to-launcher
  code needed in `main.cpp`/future `ui_task`.
- **2026-08-12** — Settings/config get persisted to a folder on the SD
  card, not flash NVS. Rationale: Launcher owns the flash partition table
  dynamically per installed app (see ROADMAP.md Distribution section), so
  a custom NVS/data partition isn't guaranteed to survive the user
  swapping firmwares. This is also just the existing DESIGN.md philosophy
  ("SD is the datastore, RAM is a relay buffer") extended to config, not a
  new pattern. No config schema/format decided yet — deferred until
  something actually needs to be configurable (Phase 2+ profile selection
  persistence being the likely first case).
- **2026-08-12** — Added `src/version.h` as the single source of truth for
  `FIRMWARE_VERSION`, printed on the boot banner. Added CI
  (`.github/workflows/build.yml`: `pio run -e cardputer-adv` +
  `pio test -e native` on every push/PR) and a tag-triggered release
  workflow (`release.yml`: `vX.Y.Z` tag -> builds, renames to
  `LoRaTraceRX-<version>.bin`, opens a **draft** GitHub Release — draft on
  purpose, since hardware verification is still pending and nothing should
  auto-publish yet). Added `[env:native]` to `platformio.ini` for
  host-based unit tests (no ESP32 toolchain needed) and a first real test
  (`test/test_channel_plans/`) validating the RF constants are in-band and
  don't collide — both verified to actually pass in this session, not just
  written. Note: `pio run` alone builds *all* environments including
  `native`, which has nothing to build outside `pio test` — always use
  `pio run -e cardputer-adv` (README/CI already do).
- **2026-08-12** — Added a rolling `dev-latest` release to `build.yml`:
  every push to `main` force-moves a `dev-latest` git tag and
  updates/republishes a prerelease at that tag with a fixed-filename
  binary (`LoRaTraceRX-dev.bin`), so there's always a stable, no-login
  download URL for SD-drop/Launcher testing without waiting on a version
  tag. Separate from `release.yml`'s versioned `vX.Y.Z` releases, which
  stay manual/deliberate. **Unverified**: this is the first time the
  workflow exists, so it hasn't actually fired on a real push yet — the
  `softprops/action-gh-release` update-in-place behavior for a re-used tag
  is a well-established pattern for this action, but confirm the first
  run actually updates `dev-latest` rather than erroring.
- **2026-08-12** — Added SD-based channel config
  (`src/config.h`/`config.cpp`, path `/loratrace/config.txt`,
  `sd-template/loratrace/config.txt` as the copyable example) so operators
  running non-default regional presets (e.g. MeshOregon) can override
  freq/SF/BW/CR without a rebuild. Scoped narrowly: one-shot boot-time SD
  read before `radio.begin()`, not the general Phase 2 settings/Logger
  architecture — this was pulled forward because it was blocking real
  testing tonight, not because Phase 1's scope changed otherwise. Values
  are bounds-checked (868-928MHz, SF5-12, CR5-8) and rejected
  field-by-field with a warning rather than applied blind, since a bad
  frequency/SF here means silently hearing nothing. Confirmed
  build-clean; SD mount itself is unverified pending the `PIN_SD_CS`
  hardware check above. This closes out the "no config schema decided
  yet" note in the previous entry.
- **2026-08-12** — Re-verified the SD/radio shared-bus finding against
  M5Stack's official docs directly (Cardputer-Adv microSD table + Cap
  LoRa-1262 SPI table, both re-fetched with a stricter verbatim-quote
  prompt after a first, hastier attempt produced a scrambled and
  internally-contradictory pin table — that error is worth remembering,
  not just fixing). User-supplied photo of the Cap LoRa-1262's printed pin
  diagram independently confirmed the SX1262 pins, the antenna-switch
  I2C address (0x43, now primary-source-confirmed, silkscreened on the
  module), and GPS UART pins. One correction found: DESIGN.md called the
  GPS chip "AT6668," official docs/diagram say "ATGM336H" — fixed, naming
  only, no pin/code impact. `board_pins.h` and DESIGN.md §1/§7 updated
  with the stronger sourcing.
- **2026-08-22** — First real-hardware boot (via Launcher SD-drop, not
  direct USB flash). Two back-to-back serial captures, read line by line:
  - Both show a clean run through to `Listening...`: antenna-switch
    IO-expander init succeeded (no FATAL hang), `SX1262 initialized.` with
    the default Meshtastic LongFast (US) channel active
    (906.875MHz/SF11/BW250/CR4:8), and no crash/reboot loop. This is the
    first empirical confirmation of I2C@0x43 register writes, FSPI
    availability, and RadioLib 7.7.x's runtime behavior on this exact
    board — see checklist above for which specific items this closes out.
  - Both also show `[config] /loratrace/config.txt not found — using
    built-in default channel.` — this is `config.cpp`'s *file-not-found*
    message, only reachable after `SD.begin(PIN_SD_CS, radioSPI)` already
    returned true (the "no SD card detected (or mount failed)" message is
    a distinct branch). That means the microSD card **mounted
    successfully** on the shared SPI bus — real hardware confirmation of
    the DESIGN.md §7 finding, not just docs. The override itself is still
    unverified since no `config.txt` was on the card.
  - The second capture starts from a true power-on and includes boot noise
    worth recording so it isn't mistaken for a firmware bug later:
    `rst:0x15 (USB_UART_CHIP_RESET)` is the normal reset every native-USB-
    CDC ESP32-S3 does when a serial terminal opens the port (DTR toggle) —
    expected given `ARDUINO_USB_CDC_ON_BOOT=1`, not a crash signature.
    `E (103) esp_core_dump_flash: Incorrect size of core dump image:
    18023945` is a stock ESP-IDF boot-time sanity check
    (`components/espcoredump`) finding non-erased, non-core-dump-shaped
    data at whatever flash offset the currently-active partition table
    calls "coredump" — non-fatal, and boot continues immediately after it.
    Likely explanation given the sideload path: Launcher owns the actual
    running partition table on a sideloaded install (ROADMAP.md
    Distribution section), which doesn't necessarily line up with this
    repo's own `default_8MB.csv`, so this offset can hold leftover bytes
    from whatever else has occupied flash there. Not produced by any code
    in this repo (confirmed via grep — no match for the message text or
    `esp_core_dump`/`esp_reset_reason` anywhere in `src/`). Likewise,
    `[boot] Turned on because (1= POWERON_RESET or 5==ESP_RST_DEEPSLEEP)
    --> 21` does not appear anywhere in this repo either — it's printed by
    Launcher itself before chain-loading `LoRaTraceRX-dev.bin`, not by our
    code (also worth noting: `21` isn't a value `esp_reset_reason_t`
    defines, reinforcing that this is Launcher's own, unrelated enum, not
    an ESP-IDF reset reason our code would ever need to interpret).
  - Net: no evidence of a firmware crash or panic anywhere in this log.
    Real gaps still open: no `[RX]` line (no live packet decoded this
    session) and the SD-config override path untested. See Next steps.
- **2026-08-22** — Investigated the "can't get back to Launcher without
  reflashing" report by cloning `bmorcelli/Launcher` (read-only) and
  reading its actual boot logic rather than guessing. Full finding
  recorded under "Open questions — Launcher distribution" above. While in
  there, also found (informational only, no action taken): Cardputer ADV
  uses a TCA8418 I2C keyboard controller (addr 0x34) on the *same* SDA/SCL
  pins (G8/G9) this repo already uses for the PI4IOE5V6408 antenna switch
  (0x43) — normal shared-I2C-bus usage, not a conflict, and our own boot
  log already shows the antenna-switch writes succeeding on real hardware.
  Their Cardputer-ADV notes separately mention GPIO5 (== `PIN_LORA_NSS`
  here) needing to be driven high during early GPIO init on that revision
  to avoid SD-mount interference from that same I2C device cluster — noted
  in `board_pins.h` for future reference, not acted on, since our own SD
  mount and radio init both already succeeded without it in the captured
  boot log.
- **2026-08-22** — `/loratrace/config.txt` is now auto-created (pre-filled
  with the current defaults) the first time this firmware sees a card that
  doesn't have one, instead of requiring an operator to hand-copy
  `sd-template/loratrace/` over themselves (`src/config.cpp`
  `writeDefaultConfig()`, gated behind `SD.exists()` so it never touches a
  file that's already there). Also fixes the noisy
  `[E][vfs_api.cpp] open(): ... does not exist, no permits for creation`
  line seen in the first hardware boot log's second capture — that came
  from opening a nonexistent file in read mode; after the first boot with
  a card, the file exists, so that path isn't hit again.
  `sd-template/loratrace/` stays around as an offline/reference copy.
  Build-clean; not yet bench-tested (needs a blank card through a real
  boot — see Phase 1 checklist).
- **2026-08-22** — Added a boot-status splash on the ST7789 LCD
  (`main.cpp` `splashLine()`/`initDisplay()`), mirroring the serial
  banner's milestones one line at a time as `setup()` reaches them,
  including the FATAL branches — so a hard failure shows on-screen too,
  not just over serial. **Explicit, narrow exception to CLAUDE.md's Phase
  1 Status text ("no UI yet")**, made at the user's direct request after
  the first hardware boot "looked like a brick" without a serial
  connection open, confirmed with the user before writing any code. Scoped
  the same way the SD-config read was: one-shot, setup()-only, no keyboard
  reading, no menus, no redraw loop — `ui_task.cpp` and real interactivity
  still wait for Phase 6. Pins, panel size, and the IPS column/row offset
  pairs (`board_pins.h`) are sourced from `bmorcelli/Launcher`'s own
  confirmed-working Cardputer/Cardputer-ADV config
  (`boards/m5stack-cardputer/platformio.ini` in that repo), not guessed —
  same sourcing discipline as the rest of this project's hardware facts,
  though still flagged TODO(verify) until bench-tested here specifically.
  Library: `moononournation/GFX Library for Arduino`, **pinned to exactly
  `1.4.0`, not caret-ranged** — actually ran `pio run -e cardputer-adv`
  (not just reasoned through) and found releases from ~1.5 onward require
  `esp32-hal-periman.h`, which only exists on Arduino-ESP32 core 3.x; this
  project's unpinned `platform = espressif32` currently resolves to core
  2.0.17, and 1.4.0 confirmed building clean against it. Also used the
  same real build to confirm both `pio run -e cardputer-adv` and `pio test
  -e native` still pass after all of tonight's changes, not just the
  display piece. Own SPI host (HSPI), pins fully disjoint from
  radioSPI/SD's FSPI pins — keeps DESIGN.md §1's bus-isolation rule intact
  rather than adding a third device to the already-shared radio/SD bus.
- **2026-08-22** — Follow-up from the boot-status splash: the splash is
  fully static once `setup()` returns (`loop()` never touches `tft`), so
  it stays on screen indefinitely showing whatever the last drawn line
  was — it does not flash briefly and disappear. That's also its weakness:
  a genuine hang (stuck in a FATAL `while(true)` loop, or wedged before
  ever reaching `loop()`) looks pixel-identical to a healthy idle screen.
  Added `heartbeatTick()` (`main.cpp`): a small dot in the bottom-right
  corner toggled every ~500ms, called from the top of `loop()` — freezes
  right alongside everything else if `loop()` stops running, which is the
  point, not a bug to fix. Still passive (no keyboard reading), so it
  doesn't cross the same CLAUDE.md Phase 6 boundary the splash itself was
  already granted an exception for.
- **2026-08-22** — Investigated "flipping the physical power switch does
  nothing, firmware just keeps running." Confirmed against M5Stack's own
  Cardputer-ADV docs (charging requires the switch ON; standby current
  with the switch OFF is ~0.23uA, i.e. genuinely off) plus a full read of
  every board-init path in `bmorcelli/Launcher`'s Cardputer/ADV code — no
  GPIO anywhere reads this switch. Conclusion, MEDIUM confidence (docs +
  absence-of-evidence in Launcher's source, not a schematic): the switch
  sits in the *battery* path only: OFF stops charging and stops the
  battery powering anything, but USB power (bench testing, exactly how
  this has been tested so far) feeds the regulator directly regardless of
  switch position, so the MCU never sees the switch move at all — not a
  firmware bug, nothing to "respect" in code, because there's no signal
  reaching the firmware to respect. Only affects standalone battery
  operation, unplugged from USB. If what's actually wanted is a
  keyboard-triggered manual sleep/power-down *while on USB* for bench
  testing, that's a different, real feature — but it needs keyboard
  reading, which is a new crossing of the Phase 6 UI boundary beyond what
  the boot splash was already granted, so it needs its own go/no-go before
  writing it, same as the splash did.
- **2026-08-22** — Investigated "Launcher goes by so fast we can't stop
  it." Same root cause as the Launcher return-to-menu finding above (the
  ~5s `bootToApp` window) — Launcher polls for a keypress every ~10ms
  (`vTaskDelay(pdMS_TO_TICKS(10))` in its input loop), so *holding a key
  down through the reset* (rather than watching the screen and reacting
  after the fact) reliably catches that window regardless of exact
  timing. Passed on to the user as the practical fix; the durable fix is
  still the "Boot to Launcher" Settings toggle documented above, once
  they've caught the window one time to reach Settings and flip it.
- **2026-08-22** — Two small additions ahead of the next hardware test
  round, packed into the same PR rather than costing a separate
  flash/test cycle: a boot-time `ESP.getFreeHeap()` snapshot (serial +
  splash), a first real data point for the open heap question above; and
  a splash line reporting whether the active channel is the hardcoded
  default or an SD override (`main.cpp`, next to the existing `[config]`
  serial messages), so the SD-config test below doesn't need a serial
  connection open to confirm it worked.
- **2026-08-22** — Direct USB flash (`pio run --target upload`, Windows/
  VSCode), first time this repo's own board def/partition table has run on
  real hardware instead of a Launcher sideload. `esptool` output: chip
  correctly identified as ESP32-S3 (QFN56) rev v0.2, Embedded Flash 8MB
  (GD), and bootloader + partition table + app all wrote and verified
  cleanly at the standard offsets (0x0/0x8000/0x10000). Note: those same
  offsets are almost certainly where Launcher's own bootloader/partition
  table lived, so this flash likely overwrote Launcher on this device
  rather than coexisting with it — not confirmed, but worth knowing before
  assuming Launcher is still reachable without reflashing it separately.
  User confirmed the firmware booted with serial output afterward; that
  specific log wasn't pasted into this session (see the Launcher SD-drop
  capture below instead), so this closes the board-def checklist item on a
  verbal user report, a lighter bar than the fully-logged boots elsewhere
  in this doc.
- **2026-08-22** — Second Launcher SD-drop test (of a build now including
  the boot splash and auto-created config), full serial capture read line
  by line:
  - `[E][esp32-hal-spi.c:215] spiAttachMISO(): HSPI Does not have default
    pins on ESP32S3!` — benign noise, not a failure: `initDisplay()` calls
    `tftSPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS)`, explicitly
    passing `-1` for MISO since the display is write-only; ESP32-S3 has no
    real HSPI/VSPI (SPI is GPIO-matrix-routed on SPI2/SPI3 hosts), so the
    Arduino core's default-pin lookup for the `HSPI` enum has nothing valid
    to report and logs this as an `E`-level line even though nothing failed
    — boot continues immediately after it and the splash goes on to render.
    Confirmed by the boot proceeding normally in the same log, not by
    reading the core's source — worth doing that read if this ever needs
    silencing rather than just explaining.
  - `Antenna switch: P0 driven high.` then immediately three
    `sdCommand(): crc error` lines, then `GO_IDLE_STATE failed` and
    `f_mount failed: (3) The physical drive cannot work` — the SD card
    failed to mount this boot, unlike the first hardware boot's clean
    mount. `[config] No SD card detected (or mount failed) — using
    built-in default channel.` confirms `config.cpp` correctly took the
    fails-safe branch rather than crashing or reading garbage. This is the
    same symptom (SD-mount interference from the shared-I2C-bus device
    cluster) that bmorcelli/Launcher's own Cardputer-ADV notes already
    flagged as needing GPIO5 (`PIN_LORA_NSS`) driven high during early GPIO
    init — recorded in `board_pins.h` on 2026-08-12 but not acted on then,
    since the first hardware boot hadn't hit it. Applied now in `main.cpp`
    `setup()`, before any I2C or SPI access: `pinMode(PIN_LORA_NSS,
    OUTPUT); digitalWrite(PIN_LORA_NSS, HIGH);`. **Hypothesis, not
    confirmed** — needs a reflash and repeat boot(s) to know whether it
    actually fixes the mount reliability or the CRC errors were coincidence
    (marginal card seating, etc.).
  - User had edited `/loratrace/config.txt` to real override values ahead
    of this boot; since the mount failed before the file was ever opened,
    the override didn't apply (expected, given the above) **and** the
    user's edits were left completely untouched on the card — confirms
    `loadChannelConfigFromSD()`'s fails-safe design does what it's supposed
    to (never touches the file it can't successfully read), this was not a
    config-parsing bug.
  - Rest of the log matched the first hardware boot: `SX1262 initialized.`,
    default channel active, `Listening...`, `Free heap: 366756 bytes` (up
    from the earlier ~360KB range boot-to-boot, consistent with normal
    heap-layout jitter, not a leak signal on its own).
  - Two further findings from this same test round, not yet root-caused —
    see their own checklist/open-question entries above: the boot splash
    doesn't clear the whole physical panel (leaves visible remnants of
    Launcher's own screen), and interrupting Launcher's boot window showed
    its screen but didn't actually stop LoRaTrace RX from continuing to
    boot.
- **2026-08-22** — Also added, confirmed with the user first since it's a
  further step past what the boot splash covered (its first genuinely
  `loop()`-updated content beyond the heartbeat dot, not just a one-shot
  `setup()` draw): a reserved "RX: none yet" splash line, overwritten in
  place with `len`/`rssi`/`snr` each time a packet decodes
  (`updateRxSplash()`). Lets the upcoming live-Meshtastic RX bench test be
  confirmed from the screen alone, no tethered laptop needed. Still one
  fixed line, no scrolling log, no keyboard/menus — the same "passive
  readout, not interactive UI" boundary the heartbeat dot and SD/heap
  status lines already sit on.

## Next steps

1. Reflash (either path — direct USB or Launcher SD-drop both now confirmed
   working) with the GPIO5/`PIN_LORA_NSS`-high-early hypothesis fix
   (`main.cpp`, applied 2026-08-22, untested) and repeat the SD-mount boot
   a few times to see whether the `crc error`/`GO_IDLE_STATE failed`
   symptom actually goes away or was unrelated (marginal card seating,
   etc.) — this is now the blocker on the SD-config-override test below.
2. Edit `/loratrace/config.txt` to a real regional preset (e.g. MeshOregon's
   values) again once SD mounting is reliable, reboot, and confirm the boot
   banner (serial and splash both) reports the overridden channel instead
   of the hardcoded default — the user's edits from the failed-mount test
   are still sitting untouched on the card, ready to retry.
3. Get a photo of the boot-splash screen-clear glitch (which edge/how much
   of Launcher's old screen stays visible) to actually root-cause it,
   rather than guessing at `board_pins.h`'s IPS offset/width/height
   constants blind.
4. Re-test the Launcher-return keypress with more care (hold through the
   reset rather than tap, per the polling-interval finding already on
   record; also try the "Boot to Launcher" Settings toggle) to figure out
   whether the "screen appears but firmware keeps booting anyway" report is
   a timing/process issue or something that needs more investigation.
5. Get the device near a known-live Meshtastic LongFast (US) transmitter
   and confirm at least one `[RX]` line with plausible RSSI/SNR — the last
   fully-unverified item in the Phase 1 checklist, and the real proof the
   antenna path is RF-correct, not just I2C-correct.
6. Start Phase 2 (task/queue architecture) only after the above land, per
   CLAUDE.md's explicit build-order instruction. Phase 2's
   radio_task/logger_task split needs to account for the shared-SPI-bus
   finding above (mount confirmed possible, not yet reliable), not just
   cross-core task separation. The boot splash added tonight stays
   setup()-only until Phase 6's `ui_task` — don't grow it into ad hoc UI
   logic in the meantime.
