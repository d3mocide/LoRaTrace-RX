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
  hypothesis fix (drive GPIO5/`PIN_LORA_NSS` high before any I2C/SPI
  access, per a note already on record in `board_pins.h`) was applied and
  merged as PR 4.
- **Update:** the next boot after that merge mounted cleanly and applied
  the SD override end-to-end — `Active channel: 918.500 MHz, SF8,
  BW125.0kHz, CR4/5`, the user's actual MeshOregon-style settings. **The
  SD-config override path is now confirmed working.** SD-mount
  reliability itself is a good sign but not fully closed — the user
  reports the earlier mount failure happened "for every bin," which points
  more toward card seating/hardware than firmware timing.
- `/loratrace/config.txt` is now auto-created (pre-filled with the current
  defaults) on the first card this firmware sees, instead of requiring an
  operator to hand-copy `sd-template/loratrace/` themselves.
- A boot-status splash now draws to the ST7789 LCD, mirroring the serial
  banner (including FATAL messages) — a narrow, one-shot exception to
  CLAUDE.md's Phase 6 UI gate, not the `ui_task`. See Decisions log for
  why, and for the researched fix to the separate "can't get back to
  Launcher" problem this was raised alongside.

**2026-08-23 update:** live `[RX]` confirmed on hardware — see Decisions
log — closing the last fully-unverified item in Phase 1's checklist. But
the packets being heard were **not** the user's own node: chasing that
surfaced two defects, one minor and one fundamental.
1. RX was re-armed too late (after Serial/display I/O), widening the
   window for missed back-to-back packets. Fixed; reflashed; **did not fix
   the symptom**.
2. **The sync word was never set at all**, so it silently inherited
   RadioLib's 0x12 default — which is *pre-1.2* Meshtastic. Current
   Meshtastic is 0x2B (verified from upstream firmware source). Since the
   SX126x only interrupts on a sync-word match, the device could not hear
   modern Meshtastic traffic at all, while still decoding unrelated gear
   sitting on 0x12. **Fixed and confirmed on hardware the same day** —
   packets now decode as well-formed Meshtastic frames (broadcast address,
   consistent channel hash, sane hop counts), including original/relay
   pairs from two identifiable nodes. See Decisions log.

**Phase 1 is functionally complete.** Radio, antenna path, SD config
override, display, and now genuine protocol-correct RX are all confirmed on
real hardware, and heap is stable under load (~338KB, no leak). The
remaining Phase 1 checklist items are bench-verification chores, not
unknowns. The last piece of *Phase 2* hardware never yet powered on is the
GPS — a standalone probe env now exists for exactly that (Next steps #4).

The device's
active channel right now is the user's own MeshOregon-style SD override
(918.5MHz/SF8/BW125/CR4:5), not the hardcoded Meshtastic US LongFast
default — worth remembering when picking a transmitter/mesh to test
against.

CI includes a rolling `dev-latest` release for grabbing a
Launcher-installable `.bin` without tagging — see Decisions log.

## Build-order checklist

Mirrors `ROADMAP.md` phases / `DESIGN.md` §9.

- [x] **Phase 0** — platformio.ini, pin map, RF tables, phase-1 code, docs
- [x] **Phase 1** — RadioLib bring-up. Builds clean, boots on hardware, SD
      config override works, and protocol-correct Meshtastic RX confirmed
      2026-08-23 (after fixing the sync word — see Decisions log). Heap
      stable under load. Remaining sub-items below are verification chores
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
  - [x] Bench-verify RX against a known live Meshtastic LongFast (US)
        transmitter — RSSI/SNR should be plausible, not just non-crashing.
        2026-08-22: device reached "Listening..." but no `[RX]` line was
        seen in that session — still open. Same day: a received packet
        now also updates a reserved splash line in place (`main.cpp`
        `updateRxSplash()`), so this can be confirmed from the screen
        alone — build-clean, untested on hardware like the rest of
        tonight's additions. **Closed 2026-08-23:** first live `[RX]`
        lines seen on real hardware — three packets (len 103/50/50, rssi
        -28/-58/-28dBm, snr ~13-14dB), plausible values, no crash. Tested
        against the user's actual MeshOregon-style channel (the SD
        override — see Current status), not the hardcoded Meshtastic
        LongFast US default, so this closes the substance of the item
        (the RX chain decodes real over-the-air LoRa packets with
        plausible RSSI/SNR) rather than its literal Meshtastic-US wording.
        See Decisions log for the follow-up "sent 3, only 1 logged"
        investigation this immediately surfaced.
  - [x] Confirm SD-based channel config actually loads: drop
        `sd-template/loratrace/` onto the SD card with real values (e.g.
        MeshOregon's), confirm the boot banner reports the overridden
        channel, not the hardcoded default. 2026-08-22: card wasn't
        carrying `config.txt` yet, only the "not found, using built-in
        default" fallback had been exercised. Same day, a later attempt hit
        an SD-mount failure before the file could even be read (see
        checklist item below and Decisions log) — override still didn't
        apply, but fails-safe held (edited file left untouched). **Closed
        2026-08-22 (after the PR 4 merge + GPIO5/NSS fix):** serial now
        shows `[config] Applied channel override from
        /loratrace/config.txt` followed by `Active channel: 918.500 MHz,
        SF8, BW125.0kHz, CR4/5` — exactly the MeshOregon-style values the
        user configured. End-to-end SD-config override path confirmed
        working on real hardware
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
        pin high before any I2C/SPI access applied in `main.cpp`.
        **Update, same day, after the fix merged (PR 4):** next boot
        mounted and read the SD card cleanly (`[config] Applied channel
        override...`, no CRC errors) — a positive data point for the fix,
        but not conclusive: per the user, the mount failure "does that for
        every bin," i.e. seemingly independent of which firmware is
        running, which points more toward card seating/a marginal card
        than firmware timing. Treat SD-mount reliability as still an open
        watch item, not fully closed, despite this clean run
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
  - [x] Fix/root-cause the boot-status splash not clearing the full
        physical panel (found 2026-08-22, second Launcher SD-drop test —
        see above and Decisions log). User photo showed the *entire lower
        portion* of the panel still displaying Launcher's own boot graphic
        untouched — not a minor border, a large chunk of the screen.
        **Root-caused against the actual GFX Library for Arduino v1.4.0
        source** (`Arduino_TFT.cpp` `setRotation()`, fetched and read, not
        guessed): `Arduino_ST7789`'s two offset pairs aren't "pair 1 =
        portrait, pair 2 = landscape" as the old `board_pins.h` comment
        assumed — at rotation 1 (what we use) `_yStart` comes from
        `COL_OFFSET2`, which `main.cpp` never supplied, so it defaulted to
        0 instead of 53. Fixed by passing all four offset constants to the
        constructor (`main.cpp`) and rewrote the `board_pins.h` comment
        with the actual per-rotation mapping. Build-clean; **not yet
        bench-tested** — needs a reflash and a new photo to confirm the
        glitch is actually gone, not just theoretically explained
- [~] **Phase 2** — task/queue architecture, GPS, SD, Logger (MVP-Beta).
      **Written 2026-08-23, not yet hardware-verified.** All three tasks,
      the cross-core queue, the shared-SPI mutex, and the batched CSV logger
      exist; 26 host tests cover every pure-logic path. What's outstanding
      is the part only hardware can answer:
  - [x] **GPS UART confirmed working on hardware 2026-08-23.** After the P0
        power fix and the pin A/B, the probe latched immediately on
        **RX=G15 / TX=G13** and streamed valid NMEA — 80 sentences per 5s,
        **zero** checksum errors, all five constellations enabled
        (GP/GL/GA/BD/GQ). `PIN_GPS_RX=15` in board_pins.h is now
        hardware-confirmed, not inferred from contradictory docs.
  - [x] **UI, battery and keyboard all confirmed on hardware 2026-08-23.**
        Three pages render, **keypress paging works** (so the TCA8418 wake
        sequence — `begin()` + `matrix(7,8)` — is correct, and the "boots
        in sleep" trap was handled). Battery reads **4.09V on USB** and
        **3.76V / 58% on battery**: both plausible LiPo values, and 58% is
        precisely what `(mv-3300)*100/800` gives for ~3765mV. A wrong
        divider would have read ~2.0V or ~8.2V, so **GPIO10 + ratio 2.0 is
        validated**, not merely sourced.
  - [x] **`ANTENNA OPEN` confirmed a false positive.** The GPS reached
        "acquiring" (satellites in view > 0) while still emitting that
        message — direct evidence the antenna works, upgrading the earlier
        MEDIUM-HIGH reasoned assessment to confirmed. The passive-ceramic
        explanation held.
  - [x] GPS produces an actual **fix** — **closed 2026-08-23**, outdoor
        ~37min deck run (`session.csv`, run 6): `fix_type=3` (3D) already
        present on the very first periodic row (uptime 64s), `ttff_s=45`,
        sats climbing 9→22 and holding a 3D fix for the entire run. The
        earlier indoor `00`-satellites/`ANTENNA OPEN` capture is now
        explained rather than just excused: same passive-antenna hardware,
        genuinely open sky this time. Antenna suspicion from that checklist
        note is retired.
  - [x] An unattended multi-hour run: detections logged with correct
        lat/lon, `qdrop`/`rowdrop` staying at zero, no heap exhaustion.
        **Closed 2026-08-23** — a second deck run (run0007, v0.2.5), this
        time genuinely multi-hour: `session.csv` covers uptime 4s->9017s,
        **2h30m**, not the "1.5 hours" it was described as when the cards
        came back (worth noting since it's the literal exit criterion —
        good that reality overshot the estimate, not the reverse). Every
        counter that matters stayed at its best possible value for the
        *entire* run, not just an interim stretch: `crc_err`/`queue_drop`/
        `bus_miss`/`row_drop`/`bus_contention` all **0** across all 151
        health rows; `sd=ok` throughout; `fix_type=3` from the first
        periodic row onward, never once dropping back to a 2D/no fix;
        `heap_free` settled at 311828B and `heap_min` at 307204B with zero
        further decline after the first two rows (no leak over 2.5 hours,
        the actual duration this checklist item asks for); `max_flush_ms`
        peaked at 40ms, `max_session_ms` at 38ms — both still comfortably
        below anything that could starve the radio. **Also resolves the
        long-standing `nmea_bad_crc` watch item**: this run ran **0.00%**
        bad-CRC across 185833 NMEA sentences (0/185833), down from ~2.0%
        on the previous (37-minute) deck run — strong evidence the v0.2.5
        GPS UART ring-buffer bump (256B->1024B) was in fact the fix, not
        just cheap insurance as it was logged at the time.
        `gps_max_loop_gap_ms` never exceeded the single 319ms value already
        present on the very first health row, meaning the GPS task was
        never meaningfully CPU-starved even under 2.5 hours of sustained
        SD-flush activity — the starvation theory that motivated adding
        that column is now the *disproven* half of the `nmea_bad_crc`
        investigation, and the buffer bump the *confirmed* fix. See the
        `detections.csv` investigation below for the one real finding this
        run surfaced — not a Phase 2 exit-criterion failure, but a data-
        quality question worth a follow-up run to close.
  - [x] *(superseded by the entry above; kept for history)* Interim
        37-minute deck run, 2026-08-23: not yet the literal exit criterion.
        judged entirely from `detections.csv`/`session.csv` per this
        checklist's own design) came back clean on every counter that
        matters: `crc_err`/`queue_drop`/`bus_miss`/`row_drop` all stayed at
        **0** for all 18 flushes across the run; `sd=ok` and
        `bus_contention=0` the entire time; `heap_free` settled flat at
        312596B and `heap_min` at 308004B with no further decline after the
        first two periodic rows (no leak signal); all 19 logged detections
        carry a fresh, plausible fix (`fix_quality=1`) with lat/lon tightly
        clustered around the deck location, matching the concurrent
        `session.csv` fixes. Battery dropped 3812mV→3740mV (72mV) over the
        run — a rate that would still leave headroom over several hours.
        **What's still open:** this is 37 minutes, not the "multi-hour"
        the criterion actually asks for — slow leaks, thermal drift, and
        SD-card long-run reliability don't reliably show up in 37 minutes.
        This run also extends the existing `nmea_bad_crc` watch item (see
        the v0.2.4 5-minute-run and run0005 Decisions log entries above)
        rather than raising a new one: those established a ~0.4-0.6%
        baseline that roughly doubled to ~1.2% during SD-flush bursts,
        but neither run had a GPS fix *and* steady detection traffic at
        the same time to test the combined condition. This run does, and
        the rate sustained **~2.0%** for the full 37 minutes (765/37707) —
        higher than either prior number, consistent with the same
        bus-contention direction those entries already flagged, not a new
        failure mode. Still non-blocking (fix type and sat count never
        degraded), but worth a dedicated look before Phase 3 adds MeshCore
        traffic on top.
  - [x] `maxflush` (worst SD bus hold) measured under real traffic —
        **closed 2026-08-23**, same deck run: `max_flush_ms` peaked at
        **29ms** across 18 flushes (`BATCH_BUF_SIZE=2048`), comfortably
        below anything that could starve the radio at Meshtastic
        inter-packet spacing. `max_session_ms` tracked identically (29ms),
        consistent with the health-row write being the loop's dominant
        cost, not a separate spike. No indication `BATCH_BUF_SIZE` needs
        shrinking.
- [ ] **Phase 3** — MeshCore profile
- [ ] **Phase 4** — `DISCOVERY_SWEEP`
- [ ] **Phase 5** — `ENERGY_SWEEP`
- [ ] **Phase 6** — UI polish, WiFi upload go/no-go

## Open questions (from DESIGN.md §7 — verify before / during build)

- [x] Meshtastic's exact SX126x sync-word register value — **resolved
      2026-08-23** from meshtastic/firmware `src/mesh/RadioLibInterface.h`
      (`const uint8_t syncWord = 0x2b;`). 0x12 turned out to be *pre-1.2*
      Meshtastic **and** RadioLib's own default, which this firmware had
      been silently inheriting. Now set explicitly in `channel_plans.h`
      and pinned by a native unit test. See DESIGN.md §7 and the Decisions
      log for the full citation. Bench-confirmation still pending
- [x] **MeshCore's** sync word — **resolved 2026-08-23** from upstream
      source. The repo has moved to `meshcore-dev/MeshCore`;
      `src/helpers/radiolib/CustomSX1262.h` calls
      `begin(LORA_FREQ, LORA_BW, LORA_SF, cr,
      RADIOLIB_SX126X_SYNC_WORD_PRIVATE, ...)` — i.e. **0x12**, RadioLib's
      stock private word. So the placeholder happened to be right and
      MeshCore RX was never broken the way Meshtastic's was. Now a named,
      cited `SYNC_WORD_MESHCORE` constant rather than a coincidence, kept
      deliberately separate from `SYNC_WORD_RADIOLIB_DEFAULT` (same number,
      different meaning) and pinned by tests
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
- [x] Real `ESP.getFreeHeap()` under load — **closed 2026-08-23 for Phase
      1**: 338496 bytes at boot and still ~338496 after sustained live RX,
      i.e. no leak in the receive path and comfortably above the paper
      estimate's low end. ROADMAP.md's "no PSRAM" risk calls now rest on a
      measured number rather than an estimate. Re-measure during Phase 2 —
      tasks, queues, and SD buffers all take their cut, and the exit
      criterion there is a multi-hour run, not a single reading.

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
- **2026-08-22** — PR 4 (GPIO5/`PIN_LORA_NSS`-high-early fix + this doc's
  updates) merged. Next Launcher SD-drop boot's serial log, read line by
  line: Launcher's own pre-boot lines (`initialize TCA8418 at address
  0x34`, `SDCARD mounted successfully`, its app-menu enumeration listing
  both `porkchop-onepork.1-6` and `LoRaTraceRX-dev (2)`) confirm Launcher
  itself is still installed and functional on this device — resolves the
  earlier open question about whether the direct-USB flash a few tests ago
  might have overwritten it. Our own firmware then mounted the SD card
  cleanly with no CRC errors, and printed `[config] Applied channel
  override from /loratrace/config.txt` followed by `Active channel:
  918.500 MHz, SF8, BW125.0kHz, CR4/5` — the exact MeshOregon-style values
  the user configured at the start of this session. **This is the first
  end-to-end confirmation of the SD-config override path**, closing that
  checklist item. Per the user, though, the earlier mount failure "does
  that for every bin" regardless of firmware — so this clean run is a good
  sign for the GPIO5 fix but not proof the underlying SD reliability
  concern is fully gone; see the caveat on the PIN_SD_CS checklist item.
  Other lines matched expected/already-explained noise: `esp_core_dump_flash:
  Incorrect size of core dump image` (different garbage number than last
  time, -20771073 vs. the earlier 18023945 — consistent with "leftover
  flash bytes at that partition offset," not a new issue) and the
  `spiAttachMISO()` HSPI warning (already explained as benign). Free heap
  338660 bytes this boot — within the same rough range as prior boots
  (~338-367KB), consistent with normal heap-layout jitter rather than a
  leak.
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

- **2026-08-22** — Root-caused and fixed the boot-splash screen-clear
  glitch, from a user-supplied photo showing Launcher's own boot graphic
  still filling the panel's lower portion behind LoRaTrace's splash text.
  Fetched and read `moononournation/Arduino_GFX`'s actual v1.4.0 source
  (`Arduino_TFT.cpp` `setRotation()`) rather than guessing at the offset
  constants: `Arduino_ST7789`'s two `(col,row)` offset-pair parameters are
  not "pair 1 for portrait rotations, pair 2 for landscape" as this repo's
  own `board_pins.h` comment previously claimed (written 2026-08-22 earlier
  the same day, based on how Launcher's confirmed-working config was read,
  not the library's own logic). The real `setRotation()` switch mixes one
  value from each pair per rotation; at rotation 1 (the only one this
  firmware uses), `_yStart` comes from `COL_OFFSET2` specifically —
  `main.cpp` only ever passed the "landscape" pair into the constructor's
  *first* slot, so `col_offset2`/`row_offset2` silently defaulted to 0,
  and `_yStart` came out 0 instead of the needed 53. `fillScreen()` then
  cleared a window that didn't line up with the panel's actual visible
  glass, leaving the mismatched region showing whatever was drawn there
  before (Launcher's graphic). Fix: pass all four offset constants
  (`TFT_COL_OFFSET_PORTRAIT`, `TFT_ROW_OFFSET_PORTRAIT`,
  `TFT_COL_OFFSET_LANDSCAPE`, `TFT_ROW_OFFSET_LANDSCAPE`, in that order) to
  the constructor in `main.cpp`; corrected the `board_pins.h` comment to
  document the library's real per-rotation mapping so this doesn't get
  re-broken later. The four numeric values themselves were already
  correct (they match the well-known offset table for this common ST7789
  135x240 IPS panel, e.g. as used in TTGO T-Display projects) — only the
  constructor call was wrong. Build-clean; **untested on hardware**, needs
  a reflash and a fresh photo to confirm.

- **2026-08-23** — First live `[RX]` lines on real hardware: three packets
  (`len=103 rssi=-28.00dBm snr=14.00dB`, `len=50 rssi=-58.00dBm
  snr=13.25dB`, `len=50 rssi=-28.00dBm snr=14.25dB`), captured on the
  user's own MeshOregon-style channel override (918.5MHz/SF8/BW125/CR4:5),
  not the hardcoded Meshtastic US default — closes the last fully-
  unverified Phase 1 checklist item (see above). Confirms the whole chain
  end-to-end on real hardware for the first time: antenna-switch path,
  FSPI wiring, DIO1 IRQ, and RadioLib's decode — and, since packets
  decoded with a clean CRC (no `[RX] read error` lines) and plausible SNR,
  implicitly confirms the sync word matches too, which CLAUDE.md flags as
  otherwise-unverified.
  - Immediate follow-up: the user sent 3 known test messages from a node
    physically next to the sniffer and only recognized 1 of the 3 logged
    lines as a match — "they just seem to be grabbing random messages."
    Two things are true at once here, and the investigation only
    confirms the first for certain:
    1. This is a promiscuous sniffer on a shared regional mesh channel by
       design (CLAUDE.md/DESIGN.md) — it will log other MeshOregon
       traffic (other nodes, relay/rebroadcast copies of the user's own
       messages) alongside the user's own sends, so a mismatch between
       "messages sent" and "lines logged" is partly expected, not
       automatically a bug. The -58dBm line among two -28dBm lines is the
       likely "someone else's/relayed packet" candidate, given the node
       under test was sitting right next to the sniffer.
    2. Root-caused a real gap in `main.cpp`'s `loop()`: `radio.startReceive()`
       to re-arm Rx Continuous ran *after* all the Serial/display I/O for
       the previous packet (multiple `Serial.print` calls plus a TFT
       `fillRect`/`print` splash update), not right after `readData()`.
       The SX1262 is out of Rx Continuous from the DIO1 IRQ until
       `startReceive()` runs again, so that whole print/draw window was a
       "deaf" period — a back-to-back packet (mesh relay traffic, or the
       user's own next test message arriving during that window) would be
       silently missed. Compounding it: the `len == 0 || len >
       sizeof(buf)` branch called `startReceive()` but never logged
       anything, so a dropped-for-bad-length packet left no trace at all,
       making "how many DIO1 events actually fired" unanswerable from the
       serial log alone.
  - Fix applied (`main.cpp` `loop()`): capture RSSI/SNR into locals and
    call `radio.startReceive()` immediately after `readData()` returns,
    *before* any Serial/display output — shrinks the deaf window to a
    couple of SPI transactions instead of the full print/draw path.
    RSSI/SNR still have to be read before the re-arm, not after:
    `GetPacketStatus` reports stats for the *last* packet, and a new one
    arriving right after `startReceive()` would overwrite them first.
    Also: the bad-length branch now logs `[RX] dropped, bad length N`
    instead of silently discarding, and successful decodes now print a
    hex dump of the payload so future correlation between "line in the
    log" and "message actually sent" doesn't have to rely on length/RSSI
    alone. This narrows the miss window; it does not eliminate it — a
    print- and draw-free path only comes with the Core-1 radio task from
    DESIGN.md §9 phase 2, which is explicitly out of scope for Phase 1.
    Build-clean by inspection (RadioLib/Arduino calls already used
    elsewhere in this file, no new APIs); **not yet bench-tested** — needs
    a reflash and a repeat of the 3-message bench test, ideally comparing
    payload hex across runs to tell "actually missed" apart from "that was
    someone else's packet" with more confidence than length/RSSI alone
    gave this session.

- **2026-08-23 (later same day)** — Reflashed with the re-arm fix above and
  re-ran the bench test: user reports "still kind of seeing a similar
  issue." So the late-re-arm timing was **a** real defect but **not the**
  root cause. Kept the fix (it's still correct, and Phase 2's radio task
  wants that ordering anyway) and went looking for the actual cause.
  - **Root cause found — wrong LoRa sync word, and this firmware never set
    one at all.** `main.cpp` called `radio.begin(freq, bw, sf, cr)` with
    only four arguments and never called `setSyncWord()`, so the sync word
    silently fell through to RadioLib's default. Verified in RadioLib's own
    `SX1262.h`: `begin(..., uint8_t syncWord = RADIOLIB_SX126X_SYNC_WORD_
    PRIVATE, ...)`, i.e. **0x12**. Verified separately in
    meshtastic/firmware `src/mesh/RadioLibInterface.h`: `const uint8_t
    syncWord = 0x2b;` — **0x2B**. The SX126x only raises an RX interrupt on
    a sync-word match, so the device was **structurally incapable of
    hearing current Meshtastic traffic**, while still happily decoding
    whatever unrelated LoRa gear sits on 0x12 (a common generic/hobbyist
    default). That is a precise, mechanical explanation for the reported
    symptom — "picks up random messages, misses my own node next to it" —
    and it explains why re-arm timing didn't move the needle.
  - Meshtastic's own source comment also **resolves DESIGN.md §7's "sources
    disagree" note** rather than just picking a side: *"For releases before
    1.2 we used 0x12 (or for very old loads 0x14). Note: do not use 0x34 -
    that is reserved for lorawan. We now use 0x2b."* 0x12 was **stale, not
    wrong** — pre-1.2 Meshtastic — which is exactly why credible-looking
    sources cite it. The separate "two-byte register mapping" ambiguity
    dissolves too: callers pass the one-byte value and RadioLib does the
    register mapping internally, and Meshtastic uses that same RadioLib
    API, so passing 0x2B matches it exactly. 0x34 → LoRaWAN independently
    corroborates DESIGN.md §6's fingerprint table.
  - Changes: added `sync_word` to `ChannelParams` and set Meshtastic's to
    a sourced `SYNC_WORD_MESHTASTIC = 0x2B` (`channel_plans.h`); passed it
    to `radio.begin()` and added it to the serial banner + splash
    (`main.cpp`); **left MeshCore explicitly on RadioLib's default with a
    TODO** rather than guessing, per CLAUDE.md. Added a `sync_word` key to
    the SD config (hex or decimal, `config.cpp`) specifically so the next
    bench test can A/B 0x2B vs 0x12 **without a reflash** — the thing this
    investigation kept wishing it had. Updated CLAUDE.md's house rule and
    DESIGN.md §6/§7 to match.
  - Verification: `pio` isn't available in this environment, so the native
    test suite was compiled and run directly against upstream Unity
    (g++, host) — **5/5 pass**, including two new ones pinning Meshtastic
    to 0x2B (and explicitly *not* 0x12/0x34) and asserting MeshCore is
    still on the default, so that test fails loudly the day someone
    resolves it. `channel_plans.h` also compiles clean under
    `-Wall -Wextra`. The firmware build itself (`pio run`) is **not**
    verified here — CI covers it.
  - **Still unproven on hardware:** that 0x2B actually recovers Meshtastic
    RX. The mechanism is solid and the values are source-verified, but
    until a reflash logs packets that correlate with deliberately sent
    messages, this is a very well-founded hypothesis, not a confirmed fix.

- **2026-08-23 (hardware) — SYNC-WORD FIX CONFIRMED. Phase 1 RX is real.**
  User pulled the branch, flashed, and captured 9 packets. Banner reads
  `918.500 MHz, SF8, BW125.0kHz, CR4/5, sync 0x2B`. This closes the
  hypothesis from the previous entry: 0x2B recovers Meshtastic RX.
  - **The payload hex proves these are genuinely Meshtastic frames**, not
    "some LoRa traffic." Decoded against Meshtastic's own `PacketHeader`
    (`src/mesh/RadioInterface.h`: `to`,`from`,`id` as 4-byte LE each, then
    `flags`,`channel`,`next_hop`,`relay_node`): every packet has a
    well-formed header, `to == 0xFFFFFFFF` (broadcast) on 8 of 9, a
    consistent channel hash `0xF7` across all of them, and `hop_limit` /
    `hop_start` in the flags byte that make sense (start 7). Random noise
    or another protocol would not produce that structure.
  - **It also retroactively explains the whole "random messages" mystery.**
    Packets arrive in *pairs* — same `from`, same packet `id`, same
    ciphertext, but `hop_limit` 7 then 6, a different `relay_node`, and a
    very different RSSI:

    | from | id | hop | relay | rssi |
    |---|---|---|---|---|
    | 0x1BBF065C | 0x2C618F2D | 7 | 0x5C | -60 |
    | 0x1BBF065C | 0x2C618F2D | 6 | 0x6A | -26 |

    That's the original transmission followed by a **mesh rebroadcast**.
    Two nodes are visible — `0x1BBF065C` (relay byte 0x5C, arriving
    -57..-61 dBm, i.e. distant) and `0x82D7776A` (relay byte 0x6A, arriving
    -16..-26 dBm, i.e. the node next to the sniffer) — and each relays the
    other's broadcasts. So a single sent message legitimately produces
    ~2 log lines with wildly different RSSI. The earlier "-58 among two
    -28s" that looked like someone else's traffic was almost certainly this
    same original/relay pairing. **A detection is not a message** — worth
    carrying into the §8 log schema and §6 fingerprinting: dedupe by
    (`from`,`id`) if a "unique messages heard" count is ever wanted, and
    `relay_node`/`hop_limit` are free topology data a wardriver should keep.
  - One packet is a **unicast**, not a broadcast: `to=0x82D7776A`
    `from=0x3B9292F1`, len 95 — a third node sending directly to the near
    node. Confirms the sniffer sees DMs, not just broadcasts (they're
    encrypted, but their *existence* and routing metadata are visible).
  - **Heap: no leak.** 338496 bytes at boot, still ~338496 after sustained
    RX. Closes the "real `ESP.getFreeHeap()` under load" open question for
    Phase 1's purposes — the number is stable under active receive, not
    just at idle. Phase 2 will need re-measuring once tasks/queues/SD
    buffers exist, but the no-PSRAM risk calls in ROADMAP.md now rest on a
    measured number instead of a paper estimate.
- **2026-08-23** — Added `src/gps_probe.cpp` + `[env:gps-probe]`: a
  standalone GPS bring-up sketch (raw NMEA passthrough, sentence counter,
  fix detection, and a 5-second heartbeat that distinguishes "no bytes at
  all" from "bytes but no fix"). Deliberately a **separate build env**, not
  a few lines in `main.cpp` — the sync-word bug is the argument for it:
  radio RX stayed broken for days partly because it was entangled with
  everything else booting, and nothing in this probe can perturb the
  now-working RX firmware because it isn't compiled into that build. Note
  `[env:cardputer-adv]` gained a `build_src_filter` excluding the probe;
  without it the two `setup()`/`loop()` pairs collide at link time. The
  NMEA field parser is hand-rolled (no TinyGPS++) so the probe can't fail
  because of a library — unit-tested on the host against real GGA/RMC
  samples under ASan+UBSan, including no-fix sentences, `*`-terminated
  fields, and out-of-range indices.

- **2026-08-23 — GPS probe found two faults, one of them self-inflicted.**
  The probe reported zero UART bytes. That was a *useful* zero: a baud
  mismatch produces garbage bytes, so silence ruled baud out immediately and
  pointed at power or pins.
  - **Primary cause: the probe never powered the GPS.** PI4IOE5V6408 P0
    doesn't only switch the RF antenna path — M5Stack's own Arduino example
    for this Cap drives expander pin 0 high to enable **GPS power**, and the
    LoRa868 Cap (which has no GPS) omits the call entirely. `main.cpp` set
    P0; the probe didn't, precisely *because* it had been written to isolate
    itself from the rest of the boot sequence. The isolation instinct was
    right; including the power rail in it was not. Extracted to
    `io_expander.cpp` and shared by both binaries so they can't drift again.
  - **Secondary: M5Stack contradicts itself on GPS RX/TX polarity.** Their
    docs PinMap reads `GPS_TX -> G13` (host receives on G13, what
    `board_pins.h` had); their tutorial's working example says
    `RXPin = 15`. Empirically G13 saw nothing. Rather than pick one and make
    an operator reflash to test the other, the probe now **A/Bs both
    orderings every 8s** and prints which produces bytes. `board_pins.h`
    adopts the example-code ordering and records the contradiction, since
    this project has already been burned once by a scraped pin table.
  - Probe also now shares `nmea.h`/`gps_parse.h` with the GPS task, so it
    validates checksums and reports real fix data instead of carrying a
    private parser — and exercises the same code the firmware depends on.
- **2026-08-23 — Phase 2 built.** Three tasks per DESIGN.md §2, a 32-deep
  cross-core queue of ~36B records, and the shared-bus arbitration that
  DESIGN.md §7 flagged as an open question before Phase 2 could start:
  - **`spi_bus.h/.cpp`** now owns both the `SPIClass` and a FreeRTOS
    **mutex** (not a binary semaphore — priority inheritance is the point:
    without it the radio task can block behind a mid-priority task that
    isn't even using SPI, and on this board that means lost packets).
  - **`radio_task`** (Core 1, prio 3) does the whole SX1262 transaction
    inside one short critical section, reading RSSI/SNR and re-arming RX
    *before* releasing — carrying forward the Phase 1 ordering fix. If the
    queue is full it drops and counts rather than waiting: a receiver that
    stalls to preserve a log row is strictly worse than one that misses it.
  - **`logger_task`** (Core 0, prio 2) batches rows into ~2KB and writes
    open/append/close per flush. Sizing rationale: the goal is **short**
    flushes, not rare ones. The SX1262 FIFO gives roughly one packet-time of
    slack, so many small bus holds beat occasional large ones — which is
    also why the buffer is 2KB and not 32KB. Open-per-flush because this
    device gets turned off by being unplugged.
  - **`gps_task`** (Core 0, prio 1) parses into a local and publishes under
    a mutex, keeping the critical section to one struct copy.
  - A stale GPS fix is treated as no fix (10s max age): at driving speed
    that's already ~250m of error, and an empty coordinate is more honest
    than a confidently wrong one. Same reasoning as refusing to render a
    no-fix as `0,0`.
  - Everything decision-shaped lives in pure headers (`detection.h`,
    `nmea.h`, `gps_parse.h`) so **26 host tests** cover it. The
    `test_detection` fixtures are the real packets captured off-air on
    2026-08-23, including an original/rebroadcast pair — so a regression in
    header parsing produces a concrete wrong answer about real traffic
    rather than an abstract failure.

- **2026-08-23 — GPS UART alive; probe reworked from firehose to instrument.**
  The A/B settled M5Stack's documentation contradiction on the first try
  (RX=G15), and the P0 power fix was the unlock. But the first successful
  run dumped ~300 lines of raw NMEA, burying the two things that actually
  mattered: `ANTENNA OPEN` and `00` satellites in view. Fixed:
  - Raw passthrough is now **time-boxed to 3s** — proof of life, not a
    monitoring mode. At ~80 sentences/5s across five constellations it's
    unreadable as a steady state.
  - **`$GxTXT` messages are surfaced on change**, with an inline note about
    the passive-antenna false positive so the next person doesn't chase it.
  - **Satellite visibility is parsed from GSV** (field 3) per constellation
    and GSA fix type (field 2). This is the diagnosis that matters once the
    UART is proven: **0 in view everywhere = sky/antenna; some in view but
    no fix = just needs time** for almanac/ephemeris. The status line now
    reads e.g. `sats=0 (GP:0 GL:0 GA:0 BD:0 GQ:0) fixtype=1`, and the advice
    branches on it instead of guessing.
  - Parsing verified on the host against the operator's actual captured
    sentences.
- **2026-08-23 — Fixed a latent static-initialisation-order bug** found by
  re-reading the Phase 2 diff adversarially rather than by any test.
  `radio_task.cpp` builds its `SX1262` at namespace scope via
  `new Module(..., sharedSpi())`, which runs during static init; `sharedSpi()`
  returned a reference to a namespace-scope global in a *different*
  translation unit, and C++ guarantees nothing about cross-TU static init
  order. It compiled and would have worked by luck (Module only stores the
  pointer, and nothing touches the bus until `setup()`), which is precisely
  what makes this class of bug expensive later. Now a function-local static,
  which is guaranteed constructed on first use. Worth noting CI could never
  have caught this — it is not a compile error.

- **2026-08-23 — UI / battery / keyboard confirmed on hardware, first try.**
  Everything sourced in the previous entry proved correct on the bench:
  - **Keyboard paging works**, which specifically validates the TCA8418
    wake sequence. That part boots in SLEEP and reports nothing until
    configured, so "keys work" is real evidence the `begin()` +
    `matrix(7,8)` path (taken from Launcher's working ADV interface) is
    right — not just that I2C is alive.
  - **Battery validated, not merely sourced**: 4.09V on USB, 3.76V / 58%
    on battery. 58% is exactly `(3765-3300)*100/800`, and both voltages sit
    in real LiPo range — a wrong divider would have produced ~2.0V or
    ~8.2V. GPIO10 and ratio 2.0 from M5Unified's `board_M5CardputerADV`
    case are correct for this board.
  - Worth recording so it isn't mistaken for a calibration bug later: on
    USB the pack reads ~4.09V, so the gauge sits just below 100% while
    charging. That's the hardware, not the maths — and since M5Stack's docs
    say charge *status* is unreadable on this board, there is no way to
    display "charging" instead.
  - **`ANTENNA OPEN` is confirmed benign.** The GPS reached "acquiring"
    (satellites in view > 0) while still emitting that message, which is
    direct evidence the antenna works. Upgrades the earlier reasoned
    MEDIUM-HIGH assessment to confirmed: the supervisor senses DC bias
    current that only an active antenna draws, and this Cap's is passive.
  - Net: of the two things flagged as unverifiable without hardware
    (battery calibration, keyboard wake), both came back correct on the
    first flash — the payoff for sourcing them from M5Unified and Launcher
    rather than guessing.

- **2026-08-23 (hardware) — GPS FIX ACQUIRED, 14 satellites.** The last
  never-proven piece of Phase 2 hardware now works end to end: expander P0
  powers the module, RX=G15 carries the NMEA, the parser validates it, and
  the receiver resolves a position. Every step of that chain had been
  reasoned about or tested in isolation; this is the first time all of them
  ran together and produced a coordinate.
  - Worth noting *which* diagnosis paid off: the probe's `sats_in_view`
    branch. "Some in view but no fix = just needs time" was the advice it
    printed, and that turned out to be exactly the situation — no further
    code change was needed between the last entry and the fix landing. The
    instrument was right, and the fix cost patience rather than debugging.
  - This closes the last **blocking** unknown for Phase 2. What remains is
    not a bug hunt but the exit criterion itself: an unattended multi-hour
    run whose logs come back clean.

- **2026-08-23 — Session health log, so an unattended run records itself.**
  With GPS working, the next gate is ROADMAP.md's Phase 2 exit criterion:
  "no dropped packets attributable to SD latency, no crash from heap
  exhaustion over a multi-hour run." Reading the firmware against that
  sentence turned up an awkward gap — **every number that settles the claim
  existed only where nobody would be looking.** `radioQueueDropCount()`,
  `loggerMaxFlushMs()`, `spiBusContentionCount()` and the heap were exposed
  precisely *because* the criterion demanded them, but they surfaced only in
  the serial status line and the on-screen pages. Both need a human present.
  A device on battery, driven for three hours and unplugged at the end,
  produced a detection log and no evidence whatsoever about its own health.
  The one run the criterion actually describes was the one run whose result
  could not be read.
  - Fix: `/loratrace/session.csv`, one row a minute plus a `reason=boot`
    row, schema in DESIGN.md §8.2, formatting in the pure header
    `session_log.h` with **8 host tests**. ~180 rows over a three-hour
    drive.
  - **`heap_min` (`ESP.getMinFreeHeap()`), not just the sample.** A
    once-a-minute reading of free heap can walk straight past a transient
    trough, and the trough is what actually ends a long run. Same reasoning
    applies to the serial line and the SYSTEM page, so both now show it too.
  - **Boot rows exist because sessions append to one file.** Without a
    marker, a power cycle mid-drive reads as the counters spontaneously
    resetting — which looks like a firmware fault instead of someone
    catching the USB cable with their knee.
  - Also records **time-to-first-fix** (`gpsFirstFixMillis()`). It is an
    operational number for a wardriver — how long after switching on the
    track becomes usable — and it can only be measured across a whole
    session, never reconstructed afterwards.
  - Deliberately *not* a second detection stream: this is instrumentation
    and it is best-effort. A lost health row gets zero retries, and the
    write is skipped entirely when SD is down, because instrumentation must
    never be the thing that ends the run it is measuring.

- **2026-08-23 — Fixed a mid-session SD remount that could never have
  worked.** Found by re-reading the logger against the "card reseated
  mid-drive" story it claims to support, not by any test. The retry path
  called `SD.begin()` — but the ESP32 Arduino core's `SD.begin()` opens with
  `if(_card) return true;`. Once a card has been mounted and then pulled,
  `_card` is still non-null, so every retry "succeeded" instantly while
  every subsequent write kept failing: `sdReady` would flip back to true,
  the next flush would fail, and the loop would spin that way forever. The
  card would never come back without a reboot, which is precisely the
  scenario the retry exists to handle.
  - `openLogsLocked()` now calls `SD.end()` first, unconditionally, so the
    remount is real. Unconditionally rather than only-on-retry so this
    function is the sole authority on the mount regardless of what the
    boot-time config read (`config.cpp`, which mounts and never unmounts)
    left behind.
  - Same species as the static-init-order bug from the Phase 2 diff: it
    compiles, it runs, and it is wrong only in the failure path — so
    neither CI nor a healthy bench session can see it. This one is worse,
    though, because the failure path is the *documented feature*.
  - The logger's two SD writers now share one `appendToFile()` helper with
    an explicit `WriteResult` (`OK` / `BUS_BUSY` / `FILE_ERROR`), since
    "bus busy, retry" and "card gone, give up" were already distinct
    behaviours that a bool return had been quietly flattening. Every bus
    hold the task takes goes through that one function, which keeps
    `maxFlushMs` honest as "the worst hold the logger caused" now that
    there is more than one file being written.


- **2026-08-23 — `rx_uptime_ms` added to `detections.csv`; the field was
  being captured and then thrown away.** Came out of asking a plain question
  of the new health log — "does this survive across runs, and is anything
  timestamped?" Both files turned out fine on persistence (append-only,
  header written only when absent, nothing truncates), but checking how a
  row gets placed in time exposed the gap: `Detection::rx_millis` is stamped
  by the radio task, crosses the queue, and carries a comment saying it
  "lets post-processing spot a stale GPS stamp caused by queue backlog" —
  and `detectionFormatCsv()` never wrote it. It reached the logger and died
  there.
  - Why it matters more than it sounds: a detection heard **before the first
    GPS fix** has an empty `timestamp_utc` *and* empty lat/lon. With uptime
    dropped, such a row had no time reference of any kind — not orderable,
    not joinable to `session.csv`, not even "40 seconds in". On a cold start
    that is every packet heard during TTFF.
  - Appended as the last column so existing parsers keep working, same rule
    as `logger_stack_free` in the session schema.
  - Absolute time comes from GPS because there is no verified RTC on this
    board (no RTC part is referenced anywhere in this project — worth
    sourcing before anyone assumes one exists). DESIGN.md §8.3 now writes
    down the consequence and the arithmetic that recovers wall-clock for a
    whole run from any single timestamped row: `timestamp_utc - uptime_s`.

- **2026-08-23 — One wardrive, one directory: `/loratrace/runNNNN/`.**
  Operator's call, and the right moment for it — changing the log layout
  *after* the Phase 2 validation drive would have invalidated that run.
  Previously both logs were single files every power-on appended to. Durable,
  but not usable: a drive is the unit an operator thinks in, and one
  continuous file turns share/import/delete/diff into text-editing chores.
  - **Indexed, not timestamped, and this is the interesting constraint.**
    The name must be chosen the moment logging starts — and at that moment
    the device does not know the time. Absolute time comes from GPS, there
    is no verified RTC on this board, and a cold TTFF is tens of seconds.
    Timestamp naming would mean either delaying file creation (losing every
    packet heard during TTFF — exactly the rows the `rx_uptime_ms` fix was
    added to rescue) or renaming later (leaving a provisional name behind on
    any power cut before the rename). An index needs no clock and is stable
    under power loss. The wall clock still reaches the card, recorded inside
    the run on the first health row with a fix.
  - **Next index comes from scanning the card, not a counter file.** The
    directory listing is the truth: it cannot drift out of sync, and there
    is no mutable state to corrupt on a power cut mid-write. Scanning for
    the *highest* index rather than the first gap means a deleted run's
    number is never silently reused by a later one.
  - **Parsing is strict — `runNNNN`, exactly four digits, nothing after.**
    That strictness is load-bearing in both directions and is what most of
    the 8 new host tests cover: too loose and `config.txt` or a stray file
    bumps the index; too strict and a real run is missed, so the next run
    reuses its number and appends into someone else's drive.
  - A card **reseated mid-drive rejoins the same run** (resolved once per
    power-on, not per mount) rather than splitting one drive across two
    folders. The gap is still recorded honestly, as `sd` going down and back
    in that run's own health rows.
  - Added a **`run` column to both CSVs**. Redundant with the directory a
    file sits in, right up until several runs are concatenated for analysis
    — at which point every row's uptime has restarted at zero and the merged
    data is silently ambiguous about which drive a row came from.
  - The run number shows on the RADIO page as `r<N>`: an operator about to
    set off wants to know the folder their data is landing in, and it is the
    one thing on that page they cannot infer from anything else.
  - **Deliberately not built yet:** an explicit start/stop gate. Today a run
    is a power-on because that is the only gate the firmware has. The
    rollover is a single internal step, so a Phase 6 UI control (or a
    profile switch) can start a new run without reshaping any of this.
  - Legacy top-level `detections.csv`/`session.csv` from earlier firmware
    are left alone on existing cards; the scan skips them by construction.

- **2026-08-23 (hardware) — v0.2.2 first boot: per-run directories work,
  and the flush metric was quietly lying.** Operator's serial capture,
  read line by line.
  - **`run=2` is the confirmation that matters.** The board had been reset
    once (the log shows two banners either side of a
    `rst:0x15 (USB_UART_CHIP_RESET)`), so the first boot created `run0001`
    and the second correctly scanned the card, found it, and claimed
    `run0002`. That exercises the whole path on real hardware: directory
    listing, `File::name()` shape, strict `runNNNN` parsing, mkdir. It also
    proves the scan **skipped `config.txt`** rather than counting it — the
    override applied on the same boot, so the file was definitely sitting
    there in the same directory being listed.
  - **`health=1 sd=ok`**: the boot health row reached the card.
  - **`flushes=0 maxflush=26ms` exposed a real defect in my own change.**
    Zero detection flushes had happened; the 26ms was the boot health row,
    charged to the detection-flush metric because both writers shared one
    high-water mark. I had written the comment claiming that was correct
    ("the worst hold the logger has caused, whichever file caused it") and
    it is — for one of the two questions this number gets asked. "Is the
    logger starving the radio?" is the max across both writers. "Is my
    batch sizing wrong?" is only ever about detection flushes. Merging them
    answered the first and silently destroyed the second, which is worse
    than useless on a metric that exists to tune the batch buffer. Now
    tracked per writer: `max_flush_ms` and `max_session_ms`, both in the
    health row and the status line.
    - Worth noting the shape of the mistake: not a crash, not a wrong
      value, but a *correct number answering the wrong question*. No test
      could have caught it, and it took one real boot printing two numbers
      side by side to make it obvious.
    - The 26ms itself is useful data, not noise: that is the first write to
      a freshly mounted card, and it is the current worst-case bus hold on
      record.
  - **`[E] spiAttachMISO(): HSPI Does not have default pins on ESP32S3!` is
    benign and is now documented in `board_pins.h` instead of being fixed.**
    The display is write-only so its bus is begun with `MISO = -1`; the core
    reads negative as "use this host's default MISO", finds S3 has none for
    HSPI, logs at ERROR and returns without attaching — which is precisely
    the desired outcome. The only way to silence it is to hand the bus a
    real GPIO as MISO, i.e. to claim a pin for a purpose it does not serve.
    A misleading pin map is a far worse legacy than a noisy boot log, and
    this project has already paid for one of those. Same category as the GPS
    `ANTENNA OPEN`: loud, alarming, correct to ignore.
  - `[W] Wire.cpp begin(): Bus already started` x2 is the documented
    deliberate re-`begin()` in `uiTaskStart()` plus the TCA8418 library
    doing the same. Harmless.
  - **Heap moved and it is worth writing down:** 317676 free after task
    start, 313068 at the first status line, `heapmin=308488`. Phase 1's
    idle number was ~338KB, so the three tasks, the queue, the UI and the
    SD buffers cost roughly 21-25KB together — comfortably inside the
    no-PSRAM budget, and the ~4.5KB gap between `heap` and `heapmin`
    already shows the trough tracking is doing its job.
  - **Still unproven, and only time fixes it:** `rx=0` and `fix=none` at the
    first status print, seconds after boot. `nmea=64 badcrc=0` says the GPS
    is talking cleanly and just hasn't fixed yet.
  - **Known wart, deliberately not papered over:** attaching a serial
    monitor toggles DTR and resets the board, so every bench session claims
    a fresh run folder holding one health row and no detections. Honest
    consequence of "a run is a power-on"; trivially identifiable and a few
    hundred bytes each. A Phase 6 start/stop gate is what actually fixes it.

- **2026-08-23 (hardware) — run0005 capture: the layout is confirmed, and
  the data exposed two more defects.** Operator pulled
  `/loratrace/run0005/` off the card. Both files present, both headers
  correct, `run=5` consistent across them, `rx_uptime_ms` populated
  (115175, 120944), two genuine Meshtastic detections
  (`!3e0c868b` -67dBm, `!69858668` -62dBm). `queue_drop`, `row_drop`,
  `bus_miss` and `bus_contention` all 0. `rows=2 flushes=2` matches `rx=2`.
  **The per-run layout, both schemas, and the whole logging path are now
  hardware-confirmed.**
  - **`logger_stack_free` paid for itself immediately.** It reports 1432
    bytes free at its worst, i.e. ~3688 used of the 5120 stack. The bump
    from 4096 was made on reasoning alone and would have left roughly 400
    bytes of headroom — uncomfortably thin under SD/FatFS. That is no
    longer an argument; it is a measurement, which was the entire point of
    logging it.
  - **Defect 1: file timestamps read 1980.** Nothing ever set the system
    clock, so it sat at the epoch and FAT stamped every file with its own
    1980 floor. A card full of runs could not be ordered by anything but
    its contents. `gps_task` now adopts GPS UTC once per power-on via
    `settimeofday()`, gated on **time alone rather than a position fix** —
    GPS has the time long before it has a fix, and this capture is the
    proof: `timestamp_utc` is populated from 15:12:29 onward while `lat`,
    `lon`, `sats` and `fix_type` all still say no fix. Waiting for position
    would have left the files wrong for that entire window.
    - Conversion is `gpsFixToEpoch()` in `gps_parse.h`, using Hinnant's
      days_from_civil rather than `mktime()`/`timegm()`: `mktime()`
      interprets its input in the process timezone and `timegm()`'s
      availability varies by libc, and the correct answer must not depend
      on whether something called `tzset()` first. Pure arithmetic, host
      tested against known timestamps including a 2028 leap day and the
      2100 non-leap-year case.
    - A plausibility floor (`GPS_MIN_PLAUSIBLE_YEAR = 2024`) gates it. This
      value stamps every file on the card, so a garbled sentence that got
      past the checksum and yielded year 2000 would silently backdate a
      whole run.
    - Known limits, documented in DESIGN.md §8.3 rather than hidden: files
      *created* before the clock is set keep their 1980 creation date (the
      run directory and both CSVs, created at mount); mtime corrects on the
      first append afterwards. A run where GPS never supplies a date stays
      1980 throughout — inherent without an RTC.
  - **Defect 2: the health log recorded the useless satellite count.**
    Every row reads `sats=0 fix_type=1`, which cannot distinguish "twelve
    satellites in view, still acquiring" from "antenna disconnected". This
    project already learned that lesson explicitly — `gps_parse.h` carries
    a comment about it, and the GPS probe was rewritten around it on
    2026-08-23 — and then the session log, the file whose whole job is
    explaining a run after the fact, shipped with only the used count.
    `sats_in_view` added alongside. The probe knew better than the logger
    did, which is a good argument for reading old lessons before writing
    new files.
  - **Watch item, not yet a problem:** `nmea_bad_crc` climbs 0 → 2 → 8
    against `nmea` 16 → 962 → 1885, about 0.4%. Low enough to be ordinary
    UART noise; worth a second look if it scales with detection traffic,
    which would point at bus or interrupt contention rather than the wire.

- **2026-08-23 (hardware) — v0.2.4, 5-minute monitored run: clock fix and
  metric split both confirmed; two things flagged for follow-up, neither
  blocking.**
  - **Clock-from-time-alone confirmed exactly as designed.**
    `[gps] system clock set from GPS: 2026-8-23 15:24 UTC` fires while the
    status line right before AND after it still reads `fix=none` — the
    clock was set with no position fix ever having landed this run. That is
    precisely the scenario the fix exists for (GPS has time long before it
    has a fix) and this is the first hardware evidence it works.
  - **`maxflush`/`maxhealth` split holds up.** `maxflush=0ms` while
    `flushes=0`, both climb together and independently afterward
    (`maxflush` 27→29ms tracking detection flushes, `maxhealth` 26→40ms
    tracking health rows on its own clock) — no more of the "flushes=0
    maxflush=26ms" contradiction from the previous boot.
  - **Zero `qdrop`/`busmiss` for the full 5 minutes**, heap flat at
    312796/308204 after initial settling, `rows`/`flushes` batching
    correctly (8 detections in 6 flushes). No regressions.
  - **Detections arrived as a burst**: `rx` 2→4→6→8 across three consecutive
    5s ticks, consistent with the original/rebroadcast pairing documented
    from the Phase 1 capture.
  - **No fix the entire run** (`fix=none` throughout, `sats` never used).
    Almost certainly bench/indoor conditions — an operator monitoring
    serial in real time is not driving. Flagged rather than assumed: this
    run does not, by itself, exercise the "GPS fix acquired" leg of
    ROADMAP.md's Phase 2 exit criterion. That still needs an outdoor run.
  - **`nmea_bad_crc` rate roughly doubled during the detection burst.**
    Baseline is ~0.5-0.6% of sentences (nmea 80→1850, badcrc 0→10). During
    the burst window, while `flushes` climbed 2→6 (nmea 2163→2900), badcrc
    went 11→20 — about 1.2%, roughly 2x baseline — then settled back to
    ~0.46% afterward (2900→5070). Small and non-blocking (GPS never had a
    fix to lose this run regardless), but a real, quantified correlation
    between active SD-flush bus activity and corrupted NMEA sentences,
    worth watching once a live wardrive has both GPS fixes and steady
    detection traffic at the same time — that is the condition this run
    couldn't test.
  - **Resolved: `run=5` on a card the operator confirms was clear of every
    run folder.** Not a scan bug — the explanation is the DTR-reset wart
    already on record. `esptool` toggles DTR/RTS around the flash itself,
    and reopening `pio device monitor` (or any terminal reconnect) does the
    same; each is a full power-on as far as `loggerTaskStart()` is
    concerned, and each claims the next index. Runs 1-4 are almost
    certainly boot-only stubs (`reason=boot`, `rx=0`, no detections) from
    flashing and reconnecting the monitor before settling in to watch the
    session that became run 5. Not verified by directly listing the card,
    but consistent with every other number in this capture and with the
    operator's own account.
    - Operator has decided to accept this rather than build the start/stop
      gate now ("let them stack") — matches what the PR already scoped as
      deliberately deferred to Phase 6. A card that fills with mostly-empty
      run folders over a bench session is the known cost of that choice; a
      few hundred bytes each, and `rx=0` makes them trivially filterable
      later if that ever matters.

- **2026-08-23 (hardware) — first outdoor, battery-only run: GPS fix closed,
  `maxflush` measured, first combined-load data point.** Run 6, ~37 minutes
  on the operator's deck, no serial console (judged entirely from the two
  CSVs the exit criterion was designed to make readable after the fact).
  - **GPS reached a 3D fix almost immediately** (`ttff_s=45`) and held it the
    entire run, sats climbing 9→22. This is the first time this board has
    ever produced a fix outdoors under real conditions — closes that
    checklist item and retires the lingering antenna suspicion from the
    indoor `00`-sats capture.
  - **`max_flush_ms` peaked at 29ms** across 18 flushes — the first real
    measurement of this number, closing that checklist item. Nowhere close
    to a level that would starve the radio; `BATCH_BUF_SIZE` doesn't need
    revisiting.
  - **Every drop counter stayed at 0 for the full run**: `crc_err`,
    `queue_drop`, `bus_miss`, `row_drop`. `sd=ok` and `bus_contention=0`
    throughout. `heap_free`/`heap_min` settled flat (312596B / 308004B)
    after the first two minutes with no further decline — no leak signal.
  - **All 19 detections carry a fresh, plausible fix** clustered tightly
    around the deck's coordinates, matching the concurrent `session.csv`
    positions row for row — the lat/lon-correctness leg of the exit
    criterion reads clean on this run.
  - **First time GPS fix + steady detection traffic coincided**, which is
    the exact combined condition the run0005 and v0.2.4 entries above
    flagged as untested for the `nmea_bad_crc`/bus-contention question.
    Answer: **~2.0%** sustained for the whole run (765/37707), above the
    ~0.4-0.6% baseline and ~1.2% burst rate seen separately before. Same
    direction as those findings, not a new failure mode, and fix quality
    never wavered — but the highest number recorded yet, so it's worth a
    closer look (a scoped burst-correlation check, not necessarily a fix)
    before Phase 3 adds a second profile's worth of traffic on top.
  - Battery 3812mV→3740mV (-72mV) over 37 minutes — a rate that leaves
    comfortable headroom over several hours, for whenever the actual
    multi-hour run happens.
  - **Not yet the exit criterion itself**: 37 minutes stationary on a deck
    is a strong, clean data point, but ROADMAP.md's Phase 2 gate is
    specifically a *multi-hour* unattended run, and this doesn't reach
    that bar on duration or on being a real drive (GPS never had to
    track movement). Phase 2 stays open pending that run.

- **2026-08-23 — v0.2.5: re-analyzed run 6 minute-by-minute, and the
  simple "SD flushes cause the noise" story doesn't hold up as cleanly as
  the earlier short bench sessions suggested; added instrumentation to
  test the actual mechanism instead of continuing to infer it.**
  - **The re-analysis.** Bucketed `session.csv` into its 37 one-minute
    intervals and split them by whether a detection flush landed in that
    minute. Windows *with* a flush: 304/13248 bad (2.29%). Windows
    *without* one: 461/24459 bad (1.88%) — a real but modest ~20% relative
    bump, not the ~2x jump the shorter v0.2.4/run0005 sessions hinted at.
    More telling: the very first three minutes, **before any flush had
    ever happened this run**, were already running 1.25-1.86% bad — close
    to this run's own "quiet" baseline and already above the ~0.4-0.6%
    baseline those earlier sessions established. Two readings of that:
    either this run's baseline noise floor is just higher for an unrelated
    reason (battery power outdoors vs. USB-tethered bench, different RF
    environment), or the once-a-minute health-row write — which fires in
    *every* interval, flush or not, so it can't be isolated by this kind
    of bucketing — is itself already enough to account for most of the
    baseline. The 60-second granularity in `session.csv` can't tell these
    apart; deciding between them needs a direct measurement, not another
    correlation on the same coarse data.
  - **Added `gps_max_loop_gap_ms`**: the worst gap the GPS task (Core 0,
    lowest priority by design — DESIGN.md §2) ever went between passes of
    its UART-drain loop. This is the actual mechanism the bus-contention
    theory depends on (a busy logger starving the GPS task long enough for
    the UART ring buffer to overflow) measured directly, rather than
    inferred from `nmea_bad_crc` moving around. If this stays small (a few
    ms) through the whole 2-hour run even during flush-heavy stretches,
    the CPU-starvation theory is wrong and the noise is coming from
    somewhere else — wiring, RF coupling off the LoRa front-end, or the
    module itself. If it spikes into the hundreds of ms alongside SD
    activity, that's the theory confirmed.
  - **Added `gps_oversize_drops`**: the line-assembly buffer (96 bytes,
    NMEA's own spec limit) has always silently discarded and resynced on
    overrun, with zero counter anywhere. A dropped byte that happens to be
    a sentence's own `\n`/`\r` merges two sentences into one, overruns the
    buffer, and vanishes without ever touching `nmea_bad_crc` — so the true
    corruption rate could be higher than that counter alone has ever shown.
  - **Bumped the GPS UART ring buffer 256B → 1024B** (`gpsSerial.
    setRxBufferSize(1024)`, before `.begin()`). At ~17 sentences/sec x
    ~75 bytes measured this run, 256B is under 200ms of slack before an
    unread buffer starts dropping bytes — cheap insurance regardless of
    what the loop-gap measurement shows, and a natural A/B against this
    run's ~2.0% number: if the rate drops substantially on the next run,
    that's independent evidence for the same starvation theory.
  - Both new fields appended after `run`, matching this schema's own
    append-at-the-end convention (`rx_uptime_ms`/`logger_stack_free` set
    the precedent) — existing tooling that reads earlier columns by
    position is unaffected. `DESIGN.md` §8.2 and `test/test_session_log/`
    updated to match; two new host tests cover the new columns' formatting
    and position, all 11 tests (54 across the full native suite) still
    pass. No `pio` in this environment — verified by compiling the native
    suite directly against upstream Unity (g++, host), same workaround
    used for the sync-word fix.
  - **Not a fix — deliberately.** Nothing about *behavior* changed except
    the buffer size; this is purely "make the next run answer the question
    the last one couldn't." The right next step is the already-planned
    2-hour deck run on this build, then reading `gps_max_loop_gap_ms`
    first, before `nmea_bad_crc` itself.

- **2026-08-23 (later same day)** — The planned run happened: run0007,
  v0.2.5, 2h30m unattended on the deck. Closes the Phase 2 multi-hour exit
  criterion and the `nmea_bad_crc` watch item — see the checklist entry
  above for the numbers. Reading `detections.csv` (110 rows) surfaced one
  real finding, worth recording in detail since it changed conclusions
  mid-investigation rather than landing on the first theory:
  - **The observation.** Grouping detections by `channel_or_node_id` and
    pairing consecutive hits from the same id within 15s of each other
    (a Python pass over the actual CSV, not eyeballing): 51 such pairs
    exist. 49 of 51 (96%) share **identical** `raw_len`. 46 of 51 (90%)
    show a >30dB RSSI swing between the two, and in every one of those 46,
    one side reads implausibly hot — specifically, 53% of *all 110*
    detections in the run carry RSSI > -25dBm, and that hot value is not
    varied noise: 39 readings sit at **exactly -7.0dBm** and 14 at
    **exactly -6.0dBm** (byte-exact on the SX126x's 0.5dB/LSB grid — bytes
    14 and 12), with only the run's final ~4 minutes drifting to a
    different but still-tight band (-11/-14/-16/-20dBm).
  - **First hypothesis, investigated and mostly ruled out: a firmware bug
    re-logging one physical packet twice.** `radio_task.cpp`'s critical
    section (`getPacketLength()` -> `readData()` -> `getRSSI()` ->
    `getSNR()` -> `startReceive()`) holds the shared-SPI mutex for the
    whole sequence, and `getRSSI()`'s no-arg default (verified against
    RadioLib 7.7.1's actual `SX126x.cpp` source, not assumed) reads packet
    -mode RSSI via `GetPacketStatus`, not the instantaneous/live-channel
    variant — so the obvious "wrong RadioLib call" theory doesn't hold up.
    No smoking gun found in `spi_bus.cpp` either (a plain FreeRTOS mutex
    held for one full transaction, not per-call). Left open, not closed:
    a stale/duplicate FIFO read from a double DIO1 fire was never
    definitively ruled out, just de-prioritized once the alternative below
    turned out to explain the *identical `raw_len`* observation for free.
  - **Second hypothesis, user-supplied and better-fitting: the user's own
    Meshtastic repeater sits within ~5 feet of the receiver during this
    test.** Link budget at 1.5m/915MHz: FSPL ≈ 20log10(1.5) +
    20log10(915) - 27.55 ≈ 35dB. A typical Meshtastic TX power (~20dBm)
    with modest antenna gains (~2dBi each end) puts received power around
    -11dBm before accounting for near-field coupling, orientation, or a
    higher TX power setting — -7dBm at 5 feet is well within physical
    plausibility, not a stretch. This explains every piece of the pattern
    at once, for free: identical `raw_len` (a Meshtastic relay preserves
    payload length — only `hop_limit`/`relay_node` change within the fixed
    header), the 2-9s gap between pairs (Meshtastic's randomized
    rebroadcast delay), the *tight* clustering at a near-constant value
    (a stationary repeater at a fixed distance should read consistently,
    not vary — the tightness that looked like a bug signature is exactly
    what real RF from a fixed-geometry relay would produce), and the
    pattern recurring across 40+ *different* origin node ids (one busy
    local repeater forwards everyone's traffic on a shared regional
    channel, not just one node's). Given this, it's the better-supported
    explanation, though not yet proven — see the fix below for how the
    *next* run settles it outright instead of by inference.
  - **The gap that made this undecidable from the log alone: routing
    metadata was parsed but never logged.** `detection.h`'s Meshtastic
    header parser has extracted `packet_id`/`hop_limit`/`hop_start`/
    `relay_node` since Phase 1 — `test_original_and_relay_share_dedupe_key`
    even exercises real captured original+relay fixtures proving
    `packet_id` matches while `hop_limit`/`relay_node` differ across a
    rebroadcast — but none of the four ever reached `detectionFormatCsv()`
    / `LOG_CSV_HEADER`. So the exact evidence needed to settle "relay" vs.
    "duplicate bug" for certain (matching `packet_id`, decremented
    `hop_limit`, different `relay_node` = relay; identical in all four =
    bug) existed in the firmware's own struct the whole time and simply
    never made it to the SD card.
  - **Fix applied, v0.2.6: wired all four into the CSV**, appended after
    `run` (same append-only-at-the-end convention `rx_uptime_ms`/
    `logger_stack_free`/the GPS diagnostic columns already established).
    Empty (not `00000000`) when no header was parsed, matching
    `channel_or_node_id`'s existing convention; `hop_limit`/`hop_start`
    stay numeric regardless since 0 is a legitimate value there, not an
    absence marker. `DESIGN.md` §8.1 updated to match. New test
    `test_csv_exposes_relay_vs_original` (`test/test_detection/`) asserts
    the CSV row itself — not just the in-memory struct — now shows the
    same `packet_id` with a decremented `hop_limit` and a different
    `relay_node` across the existing original/relay fixture pair. **No
    `pio` in this environment**, so verified the same way the sync-word fix
    was: fetched upstream Unity (ThrowTheSwitch/Unity, `master`) and
    compiled/ran all six native test binaries directly with host g++, not
    just inspection — all **55 tests pass** (12 in `test_detection`, up
    from 11; the other five suites unaffected and unchanged). Still needs
    an actual `pio test -e native` run and a reflash to confirm on the real
    toolchain/hardware. Not a behavior fix — like the GPS ring-buffer bump above, this is
    "make the next run answer the question this one couldn't," which is
    genuinely the best available move here: the alternative was guessing.

- **2026-08-23 (later same day)** — v0.2.6 shipped fast: PR #9 merged CI
  (real `pio run -e cardputer-adv` + `pio test -e native`, not just the g++
  workaround above) before the next bench session, so run0011 (a short,
  ~9-second live capture, not another multi-hour run) came back already
  carrying the new columns. **Settles the relay-vs-bug question raised by
  run0007, definitively, in favor of relay:** three packets, each heard
  twice — `packet_id` `10afda4e`/`384dfe3f` (both from `!bfbc49a2`) and
  `55f3278a` (from `!3b9292f1`) — and in every one of the three pairs,
  `packet_id` matches exactly while `hop_limit` decrements 7->6 and
  `relay_node` changes (`a2`->`5c`, `f1`->`5c`). That is precisely the
  "genuine relay" signature from the DESIGN.md §8.1 fix, and precisely NOT
  what a duplicate-log bug would produce (which would show identical
  `hop_limit`/`relay_node` too). The radio_task.cpp double-DIO1-fire
  question from the run0007 entry is now closed as a non-issue — no further
  action needed there.
  - Worth noting, not investigating further: none of run0011's 6
    detections show the extreme -6/-7dBm pegging that made up 53% of
    run0007 — these read -33 to -56dBm, all physically unremarkable. Not a
    contradiction (this is a 9-second, 3-packet sample, not a comparable
    run), but a reminder that the pegged readings themselves are still
    unexplained in *degree* even though their *mechanism* (relay, not a
    software duplicate) is now settled. `relay_node=5c` matches the last
    byte of `!1bbf065c`, one of run0007's own frequently-heard node ids —
    consistent with one specific nearby node acting as the busy relay in
    both runs, though `relay_node` is only one byte and can't fully rule
    out a different node sharing that byte.
  - **Column order reshuffled, still v0.2.6 -> now v0.2.7**, at the user's
    request after seeing the real output in a spreadsheet: the four new
    routing columns landed append-only at the end (right thing to do while
    they didn't exist yet, awkward to actually read once they did, sitting
    nowhere near the `channel_or_node_id` they describe). New order groups
    columns by what a reader asks first — when/where, then run context,
    then what-kind, then who-and-how-it-got-here (`channel_or_node_id`
    through `relay_node`, now adjacent), then RF params, then signal
    quality, then payload. Full column list and rationale: DESIGN.md §8.1.
    **This is a breaking change to column position**, called out explicitly
    in both `LOG_CSV_HEADER`'s comment and DESIGN.md: any `detections.csv`
    from before this change uses the old order, so position-based parsing
    across the boundary (e.g. concatenating run0007 with run0011+) would
    silently misalign. Every `detections.csv` still carries its own header
    row, so a reader that keys off column *names* rather than position is
    unaffected either way. `test/test_detection/` updated for the new
    layout (field mapping re-verified against the real Unity/g++ workaround
    again, all 12 tests pass) — this was purely a reorder, no column added
    or removed, so `test_header_column_count_matches_row` needed no change.
  - **Raised, not yet acted on: capturing the raw payload bytes for later
    offline decoding.** Currently `buf[256]` in `radio_task.cpp` is read,
    the 16-byte Meshtastic header is parsed out of it, and the rest
    (ciphertext) is discarded the moment the critical section ends — the
    `Detection` struct has nowhere to put it, by design (DESIGN.md §1's
    ~40B queue budget, CLAUDE.md's "no large heap buffers" rule). Genuinely
    useful for later work (MeshOregon-style channels commonly use a
    known/default PSK, so some of this may eventually be decodable
    offline), but it's a real architecture decision, not a small addition:
    growing `Detection` itself blows the documented budget across a
    32-deep queue; a second parallel queue/sidecar file keyed by
    `rx_uptime_ms` avoids that but adds a second SD writer path. Needs a
    decision on capture scope (hex column vs. separate binary sidecar file,
    full payload vs. capped length, whether MeshCore/Reticulum profiles
    even get the same treatment given CLAUDE.md's explicit warning not to
    assume MeshCore's encryption mirrors Meshtastic's) before touching
    code — not started.

## Next steps

Phase 2's own exit criterion is now closed (run0007, 2h30m, see checklist
above) — remaining items are follow-through, in the order it's worth doing.

1. **Done, 2026-08-23: the multi-hour unattended run (run0007, v0.2.5,
   2h30m).** See the Phase 2 checklist entry and the Decisions log entry
   above for the numbers — every exit-criterion counter held at its best
   value for the full run, and this also closed the `nmea_bad_crc` watch
   item (0.00% this run). What it's *not*: a moving wardrive. Motion isn't
   part of ROADMAP.md's literal exit criterion, so this doesn't block
   tagging Phase 2 done (step 4 below), but a real walking/driving drive is
   still worth doing before calling the tool field-ready — it's the only
   way to exercise two things a stationary deck run structurally can't:
   SD-card seating/contact reliability under vibration (the earlier
   CRC-mount-error watch item), and whether logged lat/lon actually tracks
   *moving* position rather than jittering around one point.
1a. **Re-run once v0.2.6 is on the device, specifically to close the
    `detections.csv` routing-metadata finding.** run0007's data was 90%
    consistent with "the nearby repeater, not a bug" but v0.2.5 didn't log
    `packet_id`/`hop_limit`/`relay_node`, so it couldn't be proven either
    way from that run alone (see Decisions log). Any run on v0.2.6 settles
    it directly: for a same-`channel_or_node_id`, near-in-time pair, a
    matching `packet_id` with decremented `hop_limit` and a different
    `relay_node` confirms genuine relay traffic; identical values across
    all three confirms the duplicate-detection theory instead and reopens
    the radio_task.cpp double-DIO1-fire question. Doesn't need its own
    dedicated run — the next session of any kind on v0.2.6 answers this.
2. **Read `session.csv` first when the card comes back**, before looking at
   the detections. In order, the questions it answers:
   - `queue_drop` and `row_drop` — must be 0. Anything else means the
     32-deep queue or the 2KB batch buffer needs revisiting, and the row
     that first went non-zero says when it started.
   - `max_flush_ms` — the worst bus hold. If drops and this both climb
     together, SD latency is starving the radio and the batch sizing is the
     lever (logger_task.h explains why the answer is *shorter* flushes,
     not rarer ones). 29ms was the worst seen on the deck run — the number
     to beat, not just a checkbox.
   - `heap_min` versus `heap_free` — a flat trough over hours is the
     no-leak result Phase 1 got at idle, now under the full task load.
     Falling means a leak, and the slope gives the rate. Flat for the first
     37 minutes; needs to stay flat for hours to actually close this.
   - `ttff_s` on the boot row, and how quickly `lat`/`lon` start appearing
     — the operational answer to "how long before the track is usable."
   - **`gps_max_loop_gap_ms` before `nmea_bad_crc`** — read this one first
     now that it exists. It says directly whether the GPS task is ever
     meaningfully CPU-starved (large gap = the bus-contention theory holds;
     small gap throughout = look elsewhere). Only then does `nmea_bad_crc`
     itself matter: ~2.0% was this run's combined-load number, `gps_
     oversize_drops` adds the previously-invisible truncation failures on
     top, and a rate that keeps *climbing* with cumulative activity rather
     than holding near 2% would upgrade this from a watch item to a bug.
3. **Spot-check `detections.csv` against the drive.** Positions should
   track the route, and the empty-lat/lon rows should cluster at the start
   (before first fix) rather than scattered through it — scattered blanks
   would mean the 10s freshness window is rejecting fixes it shouldn't. The
   deck run had a fix before the first detection, so this specific check
   (blanks-at-start-only) is still untested; a real drive's cold start will
   exercise it.
4. **Phase 2's own exit criterion is closed** (run0007, see above) — tag
   `v0.2.x`, mark the phase complete in ROADMAP.md, and start Phase 3
   (MeshCore profile) whenever it's convenient — same `HOME_LISTEN` engine,
   second channel table, sync word already sourced. Not gated on step 1a
   or step 3 above (routing-metadata confirmation and a moving wardrive are
   both genuinely follow-through, not exit-criterion blockers), but doing
   1a first is cheap and closes a real open question before it's forgotten.
5. Still-open watch items, no action unless they recur/worsen:
   - SD-mount reliability across power cycles (the GPIO5 fix's first test
     was clean, but the original report said "every bin", which points at
     card seating rather than firmware). The remount fix above now gives a
     mid-session reseat a real chance of recovering — `sd` flipping to
     `down` and back in `session.csv` is what that looks like.
   - The Launcher-return keypress: hold through the reset rather than tap,
     or use Launcher's Settings → "Boot to Launcher" toggle.
   - ~~`nmea_bad_crc` vs. bus/detection load~~ — **closed 2026-08-23**, see
     the Phase 2 checklist entry above: run0007 (v0.2.5, 2.5h) came back at
     **0.00%** (0/185833), down from ~2.0% the prior run. Strong evidence
     the ring-buffer bump was the actual fix, not just insurance.
