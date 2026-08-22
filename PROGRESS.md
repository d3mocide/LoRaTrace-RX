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
Arduino 1.4.0, 406KB flash / 20.6KB static RAM) but **not yet flashed to
hardware** — same "reasoned-through-but-unverified" status the rest of
Phase 1 carried before tonight's first boot:
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
  - [ ] Confirm `esp32-s3-devkitc-1` board def actually matches this
        board's flash/pin config (no dedicated PlatformIO board def found)
        — still open: the 2026-08-22 boot was a Launcher sideload, which
        uses Launcher's own partition table, not this repo's
        `platformio.ini`/`default_8MB.csv` (see ROADMAP.md Distribution
        section) — direct USB flash with this exact board def is still
        untested
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
        seen in that session — still open
  - [ ] Confirm SD-based channel config actually loads: drop
        `sd-template/loratrace/` onto the SD card with real values (e.g.
        MeshOregon's), confirm the boot banner reports the overridden
        channel, not the hardcoded default. 2026-08-22: card wasn't
        carrying `config.txt` yet, only the "not found, using built-in
        default" fallback has been exercised — still open
  - [x] Confirm PIN_SD_CS (12) + shared SCK40/MISO39/MOSI14 actually mount
        the SD card — now sourced directly from Cardputer-Adv + Cap
        LoRa-1262's own official docs/pin diagram (see DESIGN.md §7).
        2026-08-22: confirmed on real hardware — see Decisions log
  - [ ] Bench-verify auto-created `/loratrace/config.txt`: confirm it
        actually appears on a blank card after first boot, pre-filled with
        the current defaults, and that editing it and rebooting overrides
        the active channel — added 2026-08-22, build-clean, untested on
        hardware
  - [ ] Bench-verify the boot-status splash actually renders on the real
        ST7789 panel — pins/IPS offsets/rotation are sourced from
        bmorcelli/Launcher's confirmed-working Cardputer-ADV config, not
        independently bench-verified here (board_pins.h). Added 2026-08-22,
        build-clean, untested on hardware
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
      ROADMAP.md is provisional until this number exists

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

## Next steps

1. Flash tonight's build (`LoRaTraceRX-dev.bin` once CI republishes it, or
   direct flash) and confirm: the boot splash actually renders correctly
   on the real ST7789 (text legible, right orientation, nothing clipped —
   pins/offsets are sourced, not bench-verified); a blank SD card gets
   `/loratrace/config.txt` auto-created with the current defaults on
   first boot.
2. Try the researched Launcher fix for real: press a key during Launcher's
   ~5s boot window (or flip its Settings -> "Boot to Launcher" toggle) and
   confirm it actually lands back in the Launcher menu — closes the loop
   on the "stuck, can't switch firmware" report this session investigated.
3. Edit the now-auto-created `/loratrace/config.txt` to a real regional
   preset (e.g. MeshOregon's values), reboot, and confirm the boot banner
   (serial and splash both) reports the overridden channel instead of the
   hardcoded default — closes the one remaining untested part of the
   SD-config path.
4. Get the device near a known-live Meshtastic LongFast (US) transmitter
   and confirm at least one `[RX]` line with plausible RSSI/SNR — the
   last unverified item in the Phase 1 checklist, and the real proof the
   antenna path is RF-correct, not just I2C-correct.
5. Start Phase 2 (task/queue architecture) only after the above land, per
   CLAUDE.md's explicit build-order instruction. Phase 2's
   radio_task/logger_task split needs to account for the shared-SPI-bus
   finding above (mount confirmed; concurrent-access arbitration is not),
   not just cross-core task separation. The boot splash added tonight
   stays setup()-only until Phase 6's `ui_task` — don't grow it into
   ad hoc UI logic in the meantime.
