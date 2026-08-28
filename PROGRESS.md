# LoRaTrace RX — Progress Tracking

Living document. Update this alongside code changes — it's the "what's
actually true right now" source, `ROADMAP.md` is the "what's the plan"
source, `DESIGN.md` is the "why" source. Don't let them drift.

## Current status (2026-08-22)

**Note, 2026-08-25 doc pass:** this section is Phase 0/1 history, kept as
written per this file's own convention of not rewriting past entries. For
what's actually true today: **v0.7.0, Phases 0-7 complete and Phase 8
bounded acquisition work started**, with the
measured device budgets and soak recorded below. See
CLAUDE.md's Status section for the running narrative, or the Build-order
checklist immediately below for the live phase-by-phase state. Phase 8
(`DISCOVERY_SWEEP`) has a host/build-verified radio acquisition slice but no
hardware behavior evidence yet; Phase 9 (`ENERGY_SWEEP`) is not started; Phase 10
(Field Analyzer) is accepted as planned scope, with its v1.0 gate deferred
until Phase 9 hardware evidence exists.

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
- [x] **Phase 2** — task/queue architecture, GPS, SD, Logger (MVP-Beta).
      **Fully hardware-verified — 2026-08-25 doc pass fixed a stale marker
      here:** this header and its intro line still read "not yet
      hardware-verified" even though every sub-item below, including the
      literal 2.5-hour unattended-run exit criterion, had long since closed.
      All three tasks, the cross-core queue, the shared-SPI mutex, and the
      batched CSV logger exist and are confirmed on real hardware; 26 host
      tests cover every pure-logic path.
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
- [x] **Phase 3** — Web Command Center (WiFi AP + web UI). Built and passing
      the full host-native suite; the go/no-go heap/counter spike this phase
      was gated behind was answered (see the 2026-08-22/23 entries above).
      **Bench-verified against real hardware, 2026-08-24** (Next steps item 0
      / Decisions log): dashboard/downloads/settings all reachable from a
      real browser, connect/disconnect logging confirmed clean, and the
      Settings tab round-trip confirmed directly (`[config] Wrote ...` line
      on save, a live Meshtastic packet at exactly the saved 918.500MHz/SF8/
      BW125 after the power cycle) — this line used to say "still not
      bench-verified"; that was stale, fixed 2026-08-25 doc pass. Corrected
      numbering (2026-08-24): this checklist previously called Phase 3
      "MeshCore profile," written before WiFi got pulled forward ahead of it
      (PROGRESS.md 2026-08-22 decisions log) — ROADMAP.md's phase numbers
      are the ones that actually shipped; this list had drifted from it
  - [~] **Bench-verify the per-profile channel config split — added
        2026-08-24.** Fixes a real bug found the same day:
        the original single-preset config let a MeshCore-active Save
        silently write MeshCore's values into what the firmware would apply
        as a Meshtastic override next boot, and a profile switch (menu) back
        to a profile always reverted to its hardcoded default rather than
        keeping whatever was configured for it. Confirm on real hardware:
        editing and saving the Meshtastic preset, then the MeshCore preset,
        leaves both independently correct after a reboot (check the
        `active_profile` badge and both panels' values, `GET /api/config`);
        a mid-run profile switch (menu) picks up each profile's saved
        override rather than its hardcoded default; and the auto-created
        `/loratrace/config.txt` on a blank card actually contains both
        `meshtastic_*`/`meshcore_*` blocks pre-filled with the current
        hardcoded defaults. See PROGRESS.md's Decisions log for the full
        design and the bug this replaces.
        **Half-closed, 2026-08-25 doc pass (confirmed with the operator):**
        run0022 (2026-08-24, verbose-debug session) saved the Meshtastic
        preset via the web Settings tab, power-cycled, and confirmed the
        applied channel matched exactly (918.500MHz/SF8/BW125/CR4:5) before
        switching live to MeshCore — the Meshtastic side of the original bug
        scenario is real-hardware-confirmed. **Still open:** the MeshCore
        preset itself was never independently saved via the web UI and
        checked for persistence after a reboot/switch — the other half of
        the original bug (a MeshCore-active Save corrupting the Meshtastic
        override) was fixed in code and covered by `test_channel_plans`'s
        three new cases, but that specific save-MeshCore/reboot/verify
        sequence hasn't been run on hardware yet.
      **Operational note for this project's own SD card:** its existing
      `config.txt` (the real MeshOregon-style override from earlier
      sessions) uses the OLD unprefixed key format (`freq_mhz=`, not
      `meshtastic_freq_mhz=`) — those keys are now unrecognized and will be
      skipped with a serial warning, silently reverting that card's boot
      channel to the hardcoded Meshtastic default until the file is updated
      to the new schema (or deleted, so the firmware regenerates it fresh).
      Do this before the next bench session on that card.
- [x] **Phase 4** — MeshCore profile: same `HOME_LISTEN` engine, MeshCore
      US-narrow table wired in as a second, keyboard-switchable profile
      (DESIGN.md §5). Built 2026-08-24, passing the full host-native suite;
      **fully bench-verified 2026-08-24** — see this section's entries below
      and CLAUDE.md's Status section.
  - [x] **Live MeshCore RX bench test — closed 2026-08-24.** Real packets
        at 910.525MHz/SF7/BW62.5/CR5, RSSI -58 to -64dBm, SNR ~12dB —
        plausible and consistent across a dozen+ detections. Confirmed
        using the new verbose debug-mode serial output (see Decisions log)
        rather than a CSV pull, so this and the Phase 5 debug-mode item
        closed together in one session.
  - [x] **Mid-run switch — closed 2026-08-24, via the menu (superseding
        this item's original ~3s-hold wording — Phase 5 replaced that
        gesture).** Switched Meshtastic -> MeshCore -> Meshtastic live:
        `crc_err`/`queue_drop`/`bus_miss`/`row_drop` never moved because of
        either switch, the header/CHANNEL page updated correctly each time,
        and a real packet logged correctly on both sides — MeshCore with
        blank `node_id`/`packet_id` as designed, Meshtastic with a real
        node (`!bfbc49a2`) and two packet_ids each showing a relay
        signature (`hop_limit` decremented, `relay_node` changed between
        sightings) — the same pattern run0007 already validated as genuine
        relay traffic, not a dedup bug.
  - [x] ~~Confirm the on-screen ~3s-hold gesture doesn't fire by accident~~
        — **moot, 2026-08-24**: Phase 5 removed the hold-gesture entirely in
        favor of the menu; see that phase's own checklist entry above.
- [x] **Phase 5** — On-device menu UI. Built 2026-08-24 (digit-key page
      jumps, then the ESC/arrow-alias rework, then the ESC-opens-the-menu
      rework — see Decisions log for all three), passing the full
      host-native suite (73 tests); **fully bench-verified 2026-08-24**.
      Pulled forward ahead of `DISCOVERY_SWEEP`/`ENERGY_SWEEP` at the user's
      request — see ROADMAP.md/CLAUDE.md Status. Corrected numbering
      (2026-08-24): this checklist previously called this slot
      `DISCOVERY_SWEEP` — ROADMAP.md's phase numbers are the ones that
      actually shipped; this list had drifted from it, same kind of
      correction already made for Phase 3/4 above
  - [x] **Bench-verify Comma/Period/'1'-'5' (`src/keyboard.h`) — closed
        2026-08-24.** User confirmed on real hardware: all five digit keys
        jump to the correct page, Comma and Period move the carousel/menu
        selection as designed. This same pass is what surfaced the two
        items below — Backspace felt wrong for "back," and the operator's
        own attempt to use the keyboard's printed Fn-arrow diamond
        (expecting `;`/`/` to also navigate) found they silently did
        nothing, which is exactly the "unmapped key is safely ignored"
        allowlist property working as designed, just not the UX wanted.
  - [x] **Bench-verify the backtick/ESC key and Semicolon/Slash aliases —
        closed 2026-08-24.** Confirmed on real hardware: ESC/backtick exits
        the menu, and Semicolon/Slash move the carousel/menu selection the
        same way Comma/Period do. Same bench session then surfaced a third
        UX finding — Enter-to-open-the-menu "feels kind of weird" — fixed
        same-day (see Decisions log): ESC/backtick now *opens* the menu
        from the carousel too (closes it from the menu, same as before), so
        one key does both; Enter narrowed to "act on the highlighted menu
        row" only, no-op in the carousel. Re-verified after the change:
        ESC opens the menu, Enter is confirmed as the one that fires the
        highlighted action, ESC closes it, Enter is a no-op in the
        carousel — all four confirmed on hardware, not just built.
  - [x] Confirm the menu's two actions behave identically to their old
        gesture equivalents — closed 2026-08-24. Both exercised live:
        Profile-switch via the menu was confirmed against the CHANNEL page
        (below); WiFi toggle was confirmed via the serial log itself (see
        Phase 3 note below) rather than just the on-screen "WiFi ON" label.
  - [x] Confirm the new CHANNEL status page reflects a live profile switch
        immediately — closed 2026-08-24, confirmed on real hardware.
  - [x] Confirm the persistent footer hint lines render without
        clipping/overlap on the real 240×135 panel — closed 2026-08-24,
        confirmed on real hardware. Text itself changed this same session
        (`,/. page  1-5 jump  Enter menu` -> `` ,/. page  1-5 jump  ` menu ``)
        to match the ESC-opens-the-menu rework above; the menu's own hint
        line (`` ,/. move   Enter act   ` back ``) didn't need to change.
- [x] **Phase 6** — UI architecture redesign. Promoted ahead of
      `DISCOVERY_SWEEP` 2026-08-25 (see Decisions log below). **First cut
      built 2026-08-25 (v0.6.0)**, then **fully iterated and re-implemented
      the same day (v0.6.1)** after the operator reviewed a pixel-accurate
      HTML/Canvas mockup of the actual `ui_task.cpp` draw calls (a rolling
      preview artifact, ~9 rounds of concrete visual feedback against
      screenshots) and green-lit it for real implementation, **then
      revised again the same day (v0.6.2)** once the operator questioned
      the naming itself: branding each profile "___ Trace" made four
      settings on one sniffer read like four separate tools. See Decisions
      log below for both review histories and where the preserved
      design-reference artifact lives. Neither v0.6.1 nor v0.6.2 is a small
      fix on v0.6.0: every page was redrawn, and v0.6.2 walks the profile
      axis back to plain, unbranded names.
      **Then bench-tested against real hardware for the first time
      (v0.6.2 -> v0.6.3, 2026-08-25 evening)** — the redesign compiled and
      matched its mockup, but direct-to-panel drawing had two real failure
      modes a build report couldn't catch (full-screen flicker, toast-time
      tearing); root-caused and fixed with an off-screen
      `Arduino_Canvas_Indexed` (~32KB one-time `malloc()`, not RGB565's
      ~63KB) that every draw call now targets, blitted to the panel in one
      `tft->flush()` SPI burst. Same session also fixed a menu-label
      overflow and an inverted WiFi-toggle toast, and added Trace pause/
      standby (a real `radio.sleep(true)` battery lever, promoted to a
      root-level menu row). **Then v0.6.3 -> v0.6.4** added real PWM
      backlight control (replacing the flat digitalWrite on/off, after a
      real M5GFX-sourced non-linear-duty-curve bug was root-caused and
      fixed), a 5-100% brightness slider, a configurable idle-dim timeout,
      both persisted to SD, and genuine menu nesting (`MenuState`
      generalized to a depth-bounded recursive model, `MAX_DEPTH = 4`) for
      "System > Display > Brightness/Idle dim." See the 2026-08-25 (evening)
      and (later still) Decisions log entries below for the full sessions —
      both were bench-verified on the attached Cardputer at multiple points
      during the session, not just assumed from a clean build. **Current
      version: v0.6.4.**
      `pio test -e native` — **88/88 passed** (test_ui_menu stayed at 13,
      just its "Mesh Trace" fixture label renamed to "Profile";
      test_ui_labels shrank from 7 to 3 now that there's one flat
      `uiProfileLabel()` instead of a family/sub-profile split;
      `test_channel_plans` stays at 11, unaffected this round —
      `nextHomeListenProfile()`'s test was already removed in v0.6.1).
      `pio run -e cardputer-adv` — **SUCCESS**, confirming `ui_task.cpp`
      itself compiles clean against real Arduino/RadioLib/GFX-Library/
      TCA8418 headers (the native suite never touches this file — it needs
      Arduino.h). Static RAM 50304/327680B (15.4%), flash 957645/3342336B
      (28.7%) per the build report — build-time footprint, not a runtime
      `ESP.getFreeHeap()` reading, which stays the number that actually
      matters and is still unmeasured on real hardware:
  - [x] `ui_menu.h` — `MenuState`, the grouped root/group menu, pure and
        host-tested (`test/test_ui_menu/`, 13 tests): open/close, root and
        group PREV/NEXT wraparound, DIRECT vs. GROUP root rows, firing an
        action at either level, BACK peeling exactly one level, JUMP/NONE
        staying no-ops throughout, and selecting the "Profile" root opens
        its group with each item firing its own direct
        `SELECT_MESHTASTIC`/`SELECT_MESHCORE` action (`MenuAction` itself
        didn't change in v0.6.2 — only the root row's *label*, "Mesh
        Trace" -> "Profile", did).
  - [x] `ui_labels.h` — BRAND.md's profile/mode display strings, pure and
        host-tested (`test/test_ui_labels/`, 3 tests): one flat
        `uiProfileLabel()` (v0.6.2 — replacing v0.6.1's
        `uiTraceModeLabel()`/`uiSubProfileLabel()`/`uiActiveProfileLabel()`
        family/sub-profile split, which itself replaced an even older flat
        function of the same name) matches BRAND.md's table and none of
        the four profiles collide.
  - [x] `ui_task.cpp` fully re-rewired onto the reviewed mockup, not just
        the first pass's menu change:
    - **Root table**: both rows are GROUP — "Profile" opens onto
      Meshtastic/MeshCore (a direct pick, not Phase 4/5's cycle-on-Enter)
      and still shows the live profile on its own root row; "System"
      unchanged (WiFi, Debug). v0.6.1 briefly branded this row "Mesh
      Trace"; v0.6.2 walked that back the same day (Decisions log).
    - **Header**: profile name and page position removed (crowded the
      battery indicator on the longest BRAND.md labels); replaced by three
      always-visible status dots — GPS fix state, heap health, and a new
      RX-activity pulse that replaces the old idle heartbeat blink outright
      (the heartbeat only proved the UI task's own loop was alive, not that
      anything was actually being heard).
    - **Footer status line** (`drawFooterStatus()`, carousel only): profile
      left-anchored, page position right-anchored — where the header text
      moved to. The old persistent key-hint line (`,/. page 1-5 jump ` menu
      `) is gone; the toast band now owns that space instead.
    - **Toast**: redesigned to a flush-bottom band that slides up on show
      and carries a shrinking countdown bar — both are plain per-frame
      rectangle geometry (no alpha blending, unlike the mockup's decaying
      RX-pulse glow, which has no cheap Arduino_GFX/RGB565 equivalent and
      was ported as a binary hold-then-revert instead), driven by a bounded
      ~60ms fast-redraw burst for the toast's ~1.4s lifetime rather than a
      continuous animation loop.
    - **RADIO**: right column moved from x=132 to x=170 (matches
      GPS/CHANNEL's column start); a solid flash bar under "log" during an
      active RX pulse.
    - **CHANNEL**: redistributed down the full page height instead of
      clustered under the header; a frequency *position* bar (868–923MHz,
      the SX1262's real tuned range per DESIGN.md §1, not the full
      902–928MHz US ISM band) plus a rough time-on-air estimate
      (`estimateTimeOnAirMs()`, deliberately not spec-precise) in the right
      column.
    - **GPS**: the old dim "GP:12 GL:6…" text line replaced by 4 small
      per-constellation bars in the right column, visible in both the
      fixed and no-fix states.
    - **SYSTEM**: WIFI's carousel page is gone — merged in as a 2x2 grid's
      4th block (SSID moved into the WiFi-toggle toast message instead of
      living on this page) — and a heap-fraction bar (`drawHeapBar()`,
      against the ESP32-S3FN8's real ~512KB no-PSRAM SRAM ceiling) fills
      the space under "k heap" that used to be blank. `UiPage::COUNT` is 4
      now, not 5 (`ui_task.h`).
  - [x] **Bench-verify against the real ST7789V2 panel — closed 2026-08-25,
        across the v0.6.3/v0.6.4 sessions.** The canvas rewrite eliminated
        the flicker/tearing found on first contact with real glass (see
        entry above); the menu-label overflow and inverted WiFi-toast bugs
        were both found and fixed by using the device normally; the nested
        3-level menu (System > Display > Brightness) and its full
        breadcrumb trail were exercised reaching the brightness slider; and
        Trace pause/resume plus a profile switch fired *while* paused were
        both confirmed correctly waking and retuning the radio. **Confirmed
        with the operator (2026-08-25 doc pass):** the header's RX-pulse dot
        and RADIO page's flash bar were watched tracking live incoming
        detections on real hardware, not just reasoned through. Not
        separately re-itemized: a full one-by-one re-press of all eleven
        `keyboard.h` keys — Phase 5 already bench-confirmed each of them
        individually, and Phase 6 changed `ui_task.cpp`'s *interpretation*
        of those actions (menu depth, slider entry) rather than
        `keyboard.h`'s raw-byte decode, so a fresh per-key pass wasn't
        treated as a separate gate here.
  - [x] **WiFi+canvas heap/counter remeasurement moved to Phase 7.** The
        measurement itself remains open; the 2026-08-26 phase insertion
        promotes it from a Phase-6 loose end into the optimization epic's
        P0 hardware gate, with fragmentation and all-task stack metrics
        added instead of repeating the older free-heap-only check.
  - [x] ~~Confirm the bounded fast-redraw bursts (toast slide/countdown, RX
        pulse) don't visibly compete with SD flush timing or GPS mutex
        contention~~ — **resolved/superseded, 2026-08-25 doc pass (confirmed
        with the operator).** The mechanism this item was about no longer
        exists: v0.6.3's canvas rewrite deleted the toast-only "fast-redraw
        burst" path entirely (nothing left to protect once every frame is
        atomic) — `fullRedraw()` now runs unconditionally at every cadence,
        idle tick and animation tick alike, ending in one `tft->flush()`.
        The display is its own SPI host regardless (CLAUDE.md), so this was
        never real contention with SD's bus; the v0.6.4 session's extended
        live debugging (a background serial monitor through repeated
        brightness/menu interaction) surfaced no counter regressions either.
  - [x] **Trace pause/standby — bench-verified 2026-08-25 (v0.6.3).** A
        second radio-task mailbox puts the SX1262 into `radio.sleep(true)`
        (warm sleep) instead of continuous RX; resuming is a plain
        `radio.startReceive()`. Confirmed on real hardware: pausing/
        resuming, and — the case that actually mattered, since a stale
        pause silently swallowing an operator's profile choice would be a
        real bug — switching profiles *while* paused correctly wakes and
        retunes the radio rather than leaving it asleep on the old channel.
  - [x] **Real backlight PWM, brightness slider, idle-dim timeout, and menu
        nesting — bench-verified 2026-08-25 (v0.6.4).** The naive linear-PWM
        first attempt blacked the panel out at several duty values (root
        cause: this display needs LovyanGFX/M5GFX's exact non-linear
        256Hz/9-bit/`offset=16` curve, not a naive linear map — sourced from
        M5GFX's own `board_M5CardputerADV` code, not guessed a third time).
        With the correct curve, all 4 original presets (25/50/75/100%)
        confirmed working live via a background serial monitor watching
        `[backlight]` diagnostic lines while brightness was cycled on the
        device, both before and after the fix. The new 5-100%/5%-step
        slider, the configurable idle-dim timeout, and the "System >
        Display > Brightness/Idle dim" nested menu path were all confirmed
        working post-fix, operator-confirmed before this entry was written.
        **Not fully closed:** the 5-20% brightness range is new territory
        this session opened (the old fixed-preset menu never went below
        25%) and still needs its own dedicated low-end bench sweep — flagged
        as a watch item, not assumed safe just because the formula's intent
        is to keep low values in regulation.
  - [ ] **`ui_task.cpp` split into `ui_task.cpp`/`ui_pages.cpp`/
        `ui_actions.cpp`/`ui_task_shared.h` (2026-08-25 cleanup pass) — not
        yet bench-verified on real hardware.** A structural refactor, not a
        behavior change: `pio run -e cardputer-adv` (RAM byte-identical to
        the pre-split baseline, flash +180B) and `pio test -e native`
        (90/90) both pass, and a one-off `-Wall -Wextra` pass found no new
        warnings — see this session's own Decisions log entry for the full
        verification, including a real linker bug (a `tft` naming
        collision with `main.cpp`) the build caught and this session fixed.
        No behavior was intentionally changed, but this project's own rule
        is that a compiling build doesn't confirm real glass renders
        correctly — the next hardware session should re-run carousel
        paging, all three menu levels (root/group/slider, including the
        Brightness slider and nested Display group), Trace pause/resume,
        and idle-dim, the same checks Phase 6's v0.6.2-v0.6.4 bench passes
        already established, before trusting this split at that same
        confidence level.
- [x] **Phase 7 — Device optimization. Complete 2026-08-27 on v0.7.0.**
      The measured budgets, soak, targeted CSV-download fix, and explicit
      same-build repetition waiver are recorded in this checklist.
  - [x] Epic scope, priority order, exit criteria, and hardware matrix
        captured in ROADMAP.md Phase 7 and `HARDWARE_TESTING.md`
  - [x] P0 instrumentation implemented and locally verified: internal heap
        largest-block/block-count metrics, all five task stack watermarks,
        lifecycle `[mem]` checkpoints, and append-only `session.csv` fields.
        `pio test -e native`: 91/91; `pio run -e cardputer-adv`: SUCCESS,
        50348B static RAM / 972637B flash. Hardware evidence remains open in
        the matrix items below; a build is not a runtime measurement.
  - [x] Baseline A/B: cold boot, idle, real receive/logging, UI regression —
        **run0099 PASS, 2026-08-27.** The v0.6.8 `d3c4fe4-dirty` build held
        233,848B free / 221,172B largest block through the WiFi-off 10-minute
        idle interval, then recorded 191 received and 191 logged detections,
        157 flushes, and zero radio CRC, queue, bus, or row drops. The full
        carousel/menu/profile/pause/brightness/idle-dim UI pass was exercised
        on real glass with no visible regression. Stack minima and artifact
        hashes are preserved in
        `hardware-results/2026-08-27-run0099-baseline-ab.md`; the final row's
        169,300B WiFi-on heap is explicitly excluded from the idle baseline.
  - [x] Baseline C: 10 WiFi on/off cycles with recovery trend recorded
        - **2026-08-26 partial hardware result, run0065:** stopped after
          cycle 2 exposed a repeatable lifecycle leak. Cold WiFi-off heap
          was 233952B; the first cycle settled at the expected one-time
          framework-warm baseline of 215508B, but cycle 2 settled at
          214792B (716B lower) while allocated blocks rose from 308 to
          321. Root cause: `startAp()` called `registerRoutes()` every time,
          while this Arduino `WebServer::stop()` retains its handler list.
          Route registration is now idempotent; the required 10-cycle
          hardware retest was pending at the time of this partial result.
        - **2026-08-26 route-fix retest, run0069: PASS.** Ten complete
          client-free cycles on the replacement build settled at
          217,304–217,592B free heap (217,448B final), 167,924B largest
          block, and 20/274–277 blocks; no cumulative loss appeared and
          `queue_drop`, `bus_miss`, and `row_drop` stayed 0. The capture is
          retained in the ignored `hardware-results/private/` directory;
          boot/artifact identity was not captured because the monitor
          attached after reset.
  - [x] Baseline D: browser polling, Downloads-tab CSV retrieval, and
        settings saves — **operator-confirmed PASS on run0099, 2026-08-27.**
        The shipped web firmware exposes file retrieval through its
        `Downloads` tab; there is no separate operator-facing run-list
        control. The operator completed the dashboard polling, repeated
        detections/session downloads, and both profile settings saves on the
        same v0.6.8 build. Run 99's final session row records the expected
        WiFi-on retrieval state; the earlier short-window run0069 recovery
        concern is superseded by this accepted run and the run0081 five-minute
        client-only recovery control.
        - **2026-08-26 run0069 browser workload: finding, not accepted.** Ten
          CSV transfer checkpoints and both profile settings writes completed;
          counters stayed clean, but the first AP-off sample was 211772B /
          159732B / 27/328 versus 217448B / 167924B / 20/276 before browser
          traffic. One client reset produced socket-write errors. A later
          run0070 control rose several KB during the idle tail, so the delta
          is not yet proven persistent; repeat with a >=5-minute off-state
          settle before accepting or rejecting D.
        - **2026-08-26 run0076 client-only control: provisional finding.** A
          fresh no-client AP cycle recovered to a stable 216,744B free heap
          after five minutes; a fresh one-client/no-request cycle left about
          211KB on the device display after the same settling interval. The
          serial link dropped before the client-only `wifi-stop-after` sample,
          so largest-block/block-count recovery is missing and D remains open.
        - **2026-08-27 run0081 client-only repeat: persistent delta rejected.**
          With the full boot identity captured (`v0.6.8`, `a24abaa`), the
          client-only stop checkpoint was 209,592B free / 167,924B largest;
          it recovered to 216,624B and held there for five minutes, within
          120B of run0072's 216,744B no-client result. One late `crcerr`
          occurred, but `qdrop`, `busmiss`, and `rowdrop` stayed 0. D's browser
          workload is still open; the earlier 211KB reading was delayed
          coalescing, not a persistent client leak.
  - [x] Baseline E: WiFi+GPS+SD+UI plus real RF traffic — **operator-accepted
        early close, run0095 (2026-08-27).** Approximately 15 minutes of one-
        client WiFi/browser activity, live RF reception, GPS, SD, and UI held
        current heap near 170KB with no queue or row drops. The sole
        `busmiss=1` occurred during an operator radio-settings save and did
        not recur; `crcerr` remained 0. The operator judged this sufficient
        combined-load evidence and waived the matrix's nominal 30-minute/800-
        detection duration for this cycle. Full evidence: `hardware-results/
        2026-08-27-run0095-15min-combined-control.md`.
  - [x] Baseline F: two-hour soak — **run0102 PASS, 2026-08-27.** The
        replacement run held for 7,596s (2h06m36s) on MeshCore traffic with
        1,094 logged detections, 958 flushes, flat post-warm-up
        `heap_free=233052B` / `heap_largest=221172B`, and zero queue, bus, row,
        or NMEA CRC errors. Four radio CRC errors were accepted as transient
        RF errors. The fixed 2.5 KB transient Probe/Sweep buffer is accepted
        conditionally with no second copy, raw history, or dynamic growth.
        Full evidence and CSV hashes: `hardware-results/
        2026-08-27-run0102-baseline-f.md`.
  - [x] **Operator scope decision — no repeated full matrix on the final
        download-fix build.** The final change adds only a stack-local client
        handle, short-write/disconnect guards, and a 1ms scheduler yield; it
        adds no persistent buffer or heap-resident state. Run0102 is therefore
        the authoritative final-build heap/soak evidence, while run0108 is the
        targeted hardware validation of the changed CSV path. The strict
        one-build repetition wording in ROADMAP.md §7 is explicitly waived for
        this cycle; pre-fix/older-build A/B/E evidence remains labeled as such
        rather than being presented as a repeated final-build matrix.
  - [x] P1 task-stack right-sizing — **no-change decision.** Final measured
        minima across the soak and CSV-download validation were radio 2,116B,
        GPS 1,444B, logger 1,784B, UI 2,180B, and WiFi 3,220B. Every task
        retained more than 1KB and more than 25% of its allocated stack, so
        reducing any stack would add risk without a meaningful budget gain.
  - [x] P2 WiFi lifecycle/request optimization if the baseline identifies a
        persistent off-state or request-time cost
        - **2026-08-27 CSV-download watchdog fixed and hardware-validated.**
          Run0105 reproduced a CPU 0 WiFi-task watchdog inside the manual
          `WiFiClient::write()` CSV stream for `run0102/detections.csv`.
          The stream now rejects disconnected/short writes and yields between
          successful chunks; the same operator-triggered download completed on
          run0108 with no watchdog or reboot. Full evidence:
          `hardware-results/2026-08-27-run102-download-watchdog-fix.md`.
        - **2026-08-26 run0070 control:** a settings POST followed by an
          explicit dashboard GET produced the same initial off-state as a
          POST-only sequence (212480B vs 212464B; 159732B largest block), so
          stale request arguments are not the primary cause. The off-state
          later rose to 215856B during idle, so delayed WiFi teardown/coalescing
          remains plausible; use a >=5-minute settle and fresh no-client versus
          client-only controls before choosing a fix.
        - **2026-08-26 run0072/run0076 fresh controls:** no-client AP
          recovered to 216,744B after five minutes; one client with no
          intentional requests left about 211KB on the display after five
          minutes. This is a provisional ~5–6KB client-associated cost, but
          the serial link dropped before run0076's stop checkpoint, so the
          largest-block/block-count comparison is incomplete. P2 remains open.
        - **2026-08-27 run0081 repeat:** the client-only heap fully recovered
          to 216,624B after five minutes (120B below the no-client control),
          with no persistent largest-block loss. Do not change WiFi teardown
          or request handling based on the earlier short-window deficit; the
          remaining P2 watch item is serial-transport truncation, not a
          measured lifecycle leak.
  - [x] P3 canvas allocation — **no-change decision.** The verified indexed
        canvas remains about 32KB, while the soak held 221,172B largest free
        block and the WiFi-on evidence held at least 155,636B. No measurement
        identified the canvas as the limiting allocation, so it remains in
        place.
  - [x] Final Phase 7 close: normal/WiFi-on heap, largest-block, and task
        stack budgets are recorded; the provisional 2.5 KB transient-scan
        result buffer is conditionally accepted; hardware evidence is linked;
        and `src/version.h` is advanced to v0.7.0. The strict one-build matrix
        repetition was explicitly waived by the operator and is documented
        above rather than treated as completed evidence.

    **Final budget record:**

    | Condition | Free heap | Largest block | Evidence |
    |---|---:|---:|---|
    | Normal, post-warm-up soak | 233,052B | 221,172B | run0102 |
    | WiFi-on combined workload | 169,300B | 155,636B | run0099 |
    | Fixed transient result buffer | 2,560B | fits both margins | accepted conditionally |

    Minimum unused stack across the soak and targeted download validation was
    radio 2,116B, GPS 1,444B, logger 1,784B, UI 2,180B, and WiFi 3,220B.
- [ ] **Phase 8 — Probe / `DISCOVERY_SWEEP`. In progress (bounded radio
      acquisition slice, 2026-08-27; hardware validation remains open).** Expanded from
      the earlier outline by the accepted 2026-08-26 Phase 7–10 design in
      `research/LoRaTrace-Phases-7-10-Design.md`.
  - [x] Correct the built-in Meshtastic LongFast tuple to upstream CR 4/5;
        add the source-backed fixed candidate plan and host coverage in
        `src/discovery_plan.h` / `test/test_discovery_plan/`. Hardware RX
        verification remains open.
  - [x] Document primary-source findings and the bandwidth-specific
        Meshtastic slot formula in `research/phase8-discovery-research.md`.
  - [x] Add radio-owned bounded CAD acquisition with explicit two-symbol CAD,
        deadline polling, receive-on-hit, home restore, separate fixed
        `ScanObservation` queue, durable `probe.csv`, cumulative session
        counters, and a grouped-menu Probe trigger. Host/build verification
        is complete; hardware behavior and recovery evidence remain open.
  - [ ] Curated, versioned complete candidate tuples per profile — non-default
        Meshtastic channel-hash slots and sourced legacy MeshCore settings —
        weighted by [[meshmapper-pipeline]]'s real-world observations where
        available and including the existing per-profile home override
        (DESIGN.md §3/§9; CLAUDE.md's "Related context" note)
  - [ ] CAD `symNum` tuning bench-tested against Semtech AN1200.48 before
        trusting any false-positive/miss rate this phase reports (DESIGN.md
        §7 open item, carried since Phase 0)
  - [ ] Hardware-verify `DISCOVERY_SWEEP(profile)` radio-task state: bounded-duration
        CAD-cycles the curated candidate list, optionally opens a bounded
        receive-on-hit window, and restores the complete resolved home
        configuration on complete/cancel/timeout/failure
        (CLAUDE.md's "radio task must never block" constraint applies here
        same as everywhere else)
  - [ ] Separate fixed-size `ScanObservation` queue for non-packet CAD/energy
        measurements; keep `Detection` unchanged and put cumulative retries,
        drops, recoveries, abort reason, and home-away time in run-summary or
        health records rather than every observation
  - [ ] Durable and transient modes implemented: durable mode refuses a
        missing output file; transient mode is available only if Phase 7
        accepts the fixed 2.5 KB ceiling, reuses the live buffer, retains one
        result, stores no raw/history stream, and displays `NOT SAVED`
  - [ ] Sweep results/trigger surfaced as new entries under Phase 6's
        existing grouped menu — explicitly *not* a reason to reopen UI
        architecture a second time (DESIGN.md §9 step 8, ROADMAP.md Phase 8)
  - [ ] `detections.csv`/`session.csv` check: a `DISCOVERY_SWEEP` hit logs
        cleanly alongside `HOME_LISTEN` detections without a format change
        that breaks concatenating against already-logged runs (DESIGN.md
        §8's own "check the header before concatenating" rule)
  - [ ] Bench-verify against a real non-default-channel Meshtastic
        transmitter and/or a legacy-config MeshCore node once one is
        available to test against
  - [ ] Deterministic automated bench mode completes 1,000 Probe runs, with
        deterministic fault injection or a bench-only hook covering cancel
        at every acquisition state and proving Watch restoration
  - [ ] **Post-Phase-8 enhancement, not a v0.8 exit gate:** persistent custom
        candidate editing with a bounded schema/count, tuple validation,
        provenance, deduplication, and explicit device/web ownership
- [ ] **Phase 9 — Sweep / `ENERGY_SWEEP`. Not started.** Expanded by the
      accepted 2026-08-26 Phase 7–10 design; depends on Phase 8's proven
      observation/recovery path and Phase 7's final memory budget.
  - [ ] `ENERGY_SWEEP` implemented as its own top-level radio-task state,
        mutually exclusive with `HOME_LISTEN`/`DISCOVERY_SWEEP` (DESIGN.md
        §5) — frequency-binned RSSI sweep across the supported 868–923MHz
        range, with bounded timeout/recovery and guaranteed Watch restore
  - [ ] Two-pass acquisition: bounded energy statistics first, then LoRa CAD
        only at measured peaks, operator-selected bins, or a sparse sourced
        SF/BW subset
  - [ ] A CAD hit away from a known Meshtastic/MeshCore channel is labeled
        `unknown LoRa candidate`, never promoted to Reticulum without stronger
        evidence; energy alone is never labeled LoRa
  - [ ] Rolling-noise-floor threshold filtering for `ENERGY_SWEEP` data
        before logging — DESIGN.md §8.1 already specifies *why* (log peaks
        only, or the card fills fast for near-zero value) but the actual
        threshold logic doesn't exist yet
  - [ ] 923–928MHz front-end rolloff characterized via an empirical RSSI
        noise-floor sweep, so the UI/docs can be honest about reduced
        sensitivity in that sub-band instead of silently under-reporting
        (ROADMAP.md Phase 9, DESIGN.md §7 open item)
  - [ ] Transient Sweep shares Probe's Phase 7-gated 2.5 KB buffer ceiling,
        single-result replacement, and visible `NOT SAVED` behavior
  - [ ] Deterministic bin-to-display-column aggregation covers both endpoints
        without implying one plot pixel per stored bin
  - [ ] Reticulum and Spectrum (General Exploration) become real selectable
        entries in Phase 6's Profile menu group as `ENERGY_SWEEP` consumers;
        they do not receive fictional fixed home-channel tables
  - [ ] Known low/mid/high carriers map to correct bins, WiFi-off/on quiet
        baselines are characterized, and 24 hours of repeated sweeps show no
        leak, stack erosion, queue growth, or unrecovered radio lockup
- [ ] **Phase 10 — Field Analyzer. Planned; release gate provisional.**
      Whether this phase is required for v1.0 is decided explicitly after
      Phase 9 hardware evidence exists.
  - [ ] Meter identifies whether its value came from a packet, selected Sweep
        bin, or live Scope and displays measurement age
  - [ ] Waterfall rows come only from completed real frequency sweeps and use
        deterministic host-tested bin-to-pixel aggregation
  - [ ] Bounded radio-owned `SCOPE_ACQUIRE` samples one displayed frequency,
        shows `Watch paused`, and restores the resolved home configuration on
        complete/cancel/timeout/failure; UI code never polls the SX1262
  - [ ] Recent captures and a deterministic fixed-capacity passive node roster
        expose safe header metadata only—no payload text, keys, names, or
        transmitted coordinates
  - [ ] Incremental analyzer memory is measured against an initial 8 KB ceiling
        beyond the existing indexed canvas; no draw-loop allocation
  - [ ] Worst-case analyzer/WiFi/SD/radio stress has no drops, deadlocks, or
        watchdog resets; outdoor readability and minimum-brightness rendering
        pass as separate physical-display checks

## Open questions (from DESIGN.md §7 — verify before / during build)

- [x] Meshtastic's exact SX126x sync-word register value — **resolved
      2026-08-23** from meshtastic/firmware `src/mesh/RadioLibInterface.h`
      (`const uint8_t syncWord = 0x2b;`). 0x12 turned out to be *pre-1.2*
      Meshtastic **and** RadioLib's own default, which this firmware had
      been silently inheriting. Now set explicitly in `channel_plans.h`
      and pinned by a native unit test. Live Meshtastic reception with 0x2B
      was confirmed on hardware 2026-08-23; Phase 8 separately tracks the
      built-in LongFast CR 4/5 no-override test
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

## Changelog

Session-by-session decisions log (what changed, why, how it was
verified) moved to [CHANGELOG.md](CHANGELOG.md) (2026-08-25) — this
file had grown an unbounded rolling log at its tail past the
"Next steps" section below, past the point of staying a readable
status doc. Check CHANGELOG.md for full history; this file stays
focused on current status, the build-order checklist, open
questions, and what's next.

## Next steps

Phase 2's own exit criterion is now closed (run0007, 2h30m, see checklist
above) — remaining items are follow-through, in the order it's worth doing.

0. **Done, 2026-08-23: the Phase 3 go/no-go.** Flashed and bench-tested the
   same day it was built — see the Decisions log entry above for the real
   numbers (~55KB heap cost, exit-criterion counters held at 0 with the AP
   active, dashboard/downloads/settings all reachable from a real browser).
   **Both loose ends from that session closed 2026-08-24:** connect/
   disconnect logging confirmed clean (`[wifi] client connected, 1 total` /
   `disconnected, 0 total`, both intact) when a real phone joined and left
   the AP; and the Settings tab specifically — directly confirmed this
   time, not just inferred, via the `[config] Wrote ... reboot to apply.`
   confirmation line appearing on save and a live Meshtastic packet at
   exactly the saved 918.500MHz/SF8/BW125 after the power cycle (see this
   session's Decisions log entry, same one that shipped the Serial mutex).
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
1a. **Done, 2026-08-23: closed by run0010 (v0.3.0, WiFi AP + GPS + real
    Meshtastic traffic, ~10 min).** Settled exactly the way this item said
    it would: three `packet_id`s each seen twice within seconds, every pair
    showing `hop_limit` decremented by 1 and a different `relay_node` on
    the second sighting (`1335d28a`, `66e2811f`, `069a4065` — see Decisions
    log). Genuine relay traffic confirmed; the duplicate-detection/
    double-DIO1-fire theory is closed, not reopened.
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
   `v0.2.x`, mark the phase complete in ROADMAP.md. Not gated on step 1a
   or step 3 above (routing-metadata confirmation and a moving wardrive are
   both genuinely follow-through, not exit-criterion blockers), but doing
   1a first is cheap and closes a real open question before it's forgotten.
   **Superseded numbering note (2026-08-24):** this item originally said
   "start Phase 3 (MeshCore profile)" — written before WiFi got pulled
   forward ahead of it. MeshCore is Phase 4 in ROADMAP.md's actual numbering
   and is now built (see the Build-order checklist above); left as-is here
   rather than rewritten, since it's a historical note about what was
   planned at the time, not a live instruction.
6. **Done, 2026-08-24: Bench-verify Phase 4 (MeshCore profile).** See the
   Build-order checklist's own Phase 4 entry above for the full evidence —
   live MeshCore RX and a clean mid-run Meshtastic<->MeshCore switch, both
   closed the same session verbose debug mode shipped (item 8 below).
7. **Done, 2026-08-24: Bench-verify Phase 5 (on-device menu UI).** See the
   Build-order checklist's own Phase 5 sub-items above — all closed,
   including the ESC-opens-the-menu rework that came out of the session.
8. **Done, 2026-08-24: verbose serial debug mode.** Not a pre-planned
   checklist item — added mid-session because Phase 4's live-RX check
   needed to see real RSSI/SNR and neither the serial `[status]` line nor
   the web dashboard ever surfaces per-packet detail (only `detections.csv`
   does, which meant either an SD pull or standing up the WiFi AP just to
   download a file). A third menu row (`Debug`, `logger_task.cpp`) prints
   each detection's full CSV-shaped row to serial as it's dequeued, gated
   behind the existing per-detection format call so it can never drift from
   what actually lands on SD. Caught and fixed its own bug the same
   session: the first version teared under the same unsynchronized
   cross-core `Serial` access `main.cpp`'s `[status]` line and the `[wifi]`
   SSID bug already document — visible on real hardware as a torn debug
   line — fixed by collapsing each announcement to one buffered
   `Serial.write()`, the same pattern `main.cpp` already uses. See the
   Decisions log for the full session.
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
   - **RSSI clustering by `relay_node`, first seen run0010** (see Decisions
     log): `5c` read -6 to -10dBm every time, `68` read -60 to -66dBm every
     time, 23-of-23 and 22-of-22 across the full 29-minute run. Not an
     RSSI-read ordering bug (checked — `radio_task.cpp` already reads
     before re-arming), and too consistent within one run to be
     coincidence — reads as two specific physical relays. Downgraded from
     "watch for a pattern" to "watch whether the *same two* bands
     (`5c`/`68`, same dBm ranges) show up in future runs from this
     location," which would confirm they're fixed nearby nodes rather than
     something specific to this session.
   - **New, 2026-08-24: a single-write truncation that survives the Serial
     mutex fix (`serial_lock.h`), rate unknown but low.** One
     `writeProfileConfigToSD()` confirmation line came out as `18.500MHz
     SF8 BW125.0 CR4/5 sync 0x2B — reboot to apply.` — missing its
     `[config] Wrote /loratrace/config.txt (meshtastic): 9` prefix
     entirely — during the same re-test session that confirmed the mutex
     fix works (three other messages immediately around it, including
     another `[config]` write for MeshCore, came through perfectly intact).
     This rules out the cross-task interleaving the mutex was built to
     stop: the `[status]` line printed immediately before it was itself
     complete, with nothing extra appended, so no other task's output
     landed inside this one's critical section — the mutex was almost
     certainly held for the whole call. Reads instead like the USB-CDC
     write itself dropping the front of a single locked call under load
     (this happened right as a WiFi client was actively saving Settings,
     i.e. real WebServer/WiFi-driver activity competing for the USB TX
     path) — a lower-level driver question the tools available in this
     session can't chase further (would want a logic analyzer on D+/D-, or
     at minimum a lot more controlled reproduction). Left as a watch item
     rather than dug into further given the hour; if it recurs, worth
     trying an explicit `Serial.flush()` after the locked write, or a
     larger USB-CDC TX buffer, as a first mitigation to test.
   - **2026-08-27 recurrence, run0081:** the raw capture still contained
     malformed individual lines during the AP/client burst (`wifi-start-before`,
     the AP-start announcement, and one client event), despite the cable being
     stable and the software mutex held. The stop memory checkpoints and later
     status lines were complete. Treat serial text as incomplete evidence until
     the proposed `Serial.flush()`/TX-buffer mitigation is tested; do not infer
     a WiFi lifecycle failure from a missing log line alone.
   - **2026-08-27 mitigation flashed and exercised in run0095:** runtime
     diagnostics now use a complete-write helper, the native USB-CDC TX ring
     is 1KB (up from the 256B default), and `SerialLock` avoids the driver's
     disconnected-queue discard path. The GPS clock line and AP start/stop
     announcements arrived complete; long pre-operation/config lines still
     occasionally clipped under WebServer activity, so serial text remains
     supplemental to `session.csv` and status counters. The build is 50,348B
     static RAM / 972,573B flash; native tests remain 91/91.
