# Heltec single-button menu and beacon mode — research note

**Status:** implemented and partially hardware-verified (2026-08-28). The
PRG button's hold-to-trigger behavior was physically confirmed by the
operator for the prior single-purpose sweep trigger; the new multi-screen
menu built on top of that same button has not yet been physically walked
through screen-by-screen. Everything reachable over serial has been
verified against the attached Heltec. Bench exploration, not a Phase 8/9
exit-gate item. Builds on `research/heltec-standalone-sweep.md`, which
covers the `SWEEP` command this menu now also exposes as a screen.

## Context

Following the standalone-sweep work, the operator physically confirmed the
PRG-button long-press trigger works, then asked for two things in the same
session:

1. A **field beacon mode** — periodic transmissions for verifying
   Cardputer detection/range with a second device, no cable required.
2. A proper **on-device menu**, since the Heltec has exactly one usable
   button (PRG/GPIO0) and was starting to accumulate more than one
   button-triggerable mode (sweep, now beacon, likely more later). The
   operator specifically suggested leaning on Meshtastic's own convention:
   short press cycles screens, long press acts on whichever screen is
   showing.

Both landed in the same change because the beacon needed the menu anyway —
a single hardcoded "hold 2s" trigger stops working the moment there's a
second thing you might want to hold the button for.

## Design

### Menu

Five screens, cycled in a fixed loop by a short press/release (`< 2000ms`)
and acted on by a long press (`>= 2000ms` held), mirroring Meshtastic's
single-button convention as suggested:

| Screen | Long-press action |
|---|---|
| `HOME` | none — status overview only |
| `CANDIDATE` | advance to the next compiled candidate tuple |
| `FIRE` | arm and fire one pulse at the active candidate (250ms delay) |
| `SWEEP` | run the standalone RSSI sweep (see the sibling research note) |
| `BEACON` | start the beacon if idle, stop it if running |

`2000ms` was kept as the long-press threshold specifically because it's
the exact duration the operator had already physically confirmed working
for the previous (single-purpose) hold-to-sweep behavior — no new timing
to re-learn or re-verify from scratch.

Every screen action re-checks its own busy-guard (`!armed && !beaconActive`,
etc.) rather than trusting that the button path is somehow more privileged
than a serial command — the same guards `ARM`/`SWEEP`/`BEACON` already
enforce over serial apply identically to a button-triggered action, so
there's exactly one set of rules, not two.

### Beacon mode

Periodic capped-count TX pulses at whatever candidate is currently active:

| Parameter | Value | Rationale |
|---|---|---|
| Interval | 2000ms | Slow enough to read clearly as a test signal on the Cardputer's detection UI, not a flood |
| Cap | 120 pulses (~4 minutes) | Bounded by *count*, not just a timer, so a beacon can't be left running unattended if the operator walks out of button/serial reach — same bounded-duration ethos as `ARM_DELAY_MAX_MS`'s 5-second cap elsewhere in this same firmware |
| Power | -9dBm (unchanged `TX_POWER_DBM`) | Beacon reuses the existing capped output setting; nothing about this mode raises transmit power |

Mutual exclusion is symmetric: `ARM` and `SWEEP` both refuse with
`ERROR BEACON_ACTIVE` while the beacon runs, and `BEACON START` refuses
with `ERROR ARMED` if a manual pulse is already armed. `QUIET` stops both
an armed pulse and an active beacon in one call, so there's a single
"make it stop" command regardless of which mode is running. Beacon pulses
are logged as plain `#BEACON <n> <candidate> <freq_khz> <OK|ERROR>` serial
lines rather than framed protocol responses, since they're not responses
to any one host request — `BEACON START`/`STOP` still get proper framed
ACKs. `STATUS` gained a trailing `;B=<0|1>` beacon-active field.

### A documented boundary, deliberately superseded

`research/phase8-low-profile-harness-design.md` states plainly: "the
fixture is closed and low-power. It is not an over-the-air field test
transmitter." Beacon mode is exactly that — an over-the-air field
transmitter — so this isn't a gap I found and quietly filled; it's a
boundary I'd previously cited as a reason *not* to build a walking-beacon
idea a few hours earlier in this same session. The operator's follow-up
message ("I am in alignment for you mobile testing mode the beacon...
would be super useful in the field") is explicit authorization to cross
that specific line for this specific mode, so I amended that document
in place with a dated note rather than leaving it silently contradicted.
The exception is narrow: it applies to `BEACON` only, not to Probe/CAD
timing, fault injection, or any other fixture-dependent Phase 8/9 work,
which all still require the closed fixture as documented. Power stays
capped at the same -9dBm as every other mode; the pulse-count cap (not a
pure timer) is the concrete safety property that makes "untethered" not
mean "unbounded."

## Hardware verification (real device, `/dev/ttyACM0`)

Built clean (`pio run -e heltec-v4r8`), flashed, then driven over the real
framed protocol:

```text
--- baseline ---
@LTTX/1 1 ACK V4R8;SX1262;MAXM9 C2EC
@LTTX/1 2 STATUS C=LONG_FAST;A=0;SW=NONE;B=0 50DB
--- beacon start ---
@LTTX/1 3 ACK BEACON_STARTED F286
#BEACON START
#BEACON 1 LONG_FAST 906875 OK
@LTTX/1 4 STATUS C=LONG_FAST;A=0;SW=NONE;B=1 B77C
--- ARM should be rejected (beacon active) ---
@LTTX/1 5 ERROR BEACON_ACTIVE 51D0
--- SWEEP should be rejected (beacon active) ---
@LTTX/1 6 ERROR BEACON_ACTIVE 3FEB
--- watch a couple of beacon pulses on plain serial ---
#BEACON 2 LONG_FAST 906875 OK
#BEACON 3 LONG_FAST 906875 OK
--- beacon stop ---
@LTTX/1 7 ACK BEACON_STOPPED DA4A
#BEACON STOPPED
@LTTX/1 8 STATUS C=LONG_FAST;A=0;SW=NONE;B=0 5870
--- double stop should error ---
@LTTX/1 9 ERROR NOT_ACTIVE 3D7B
--- now ARM should work again ---
@LTTX/1 10 ACK ARMED CC2E
@LTTX/1 10 TX_STARTED LONG_FAST 50AD
@LTTX/1 10 TX_DONE OK A8B9
```

The pre-existing `SWEEP` command was re-run after the refactor and still
completes a full 221-bin sweep correctly (`SWEEP_DONE P=0/221`), confirming
the menu rework didn't regress it.

## Open items

- ~~Screen cycling and per-screen long-press actions not yet physically
  walked through~~ — **confirmed working by the operator on real
  hardware** (2026-08-28). Two follow-on changes landed the same day,
  requested after that test: the long-press threshold moved 2.0s → 2.5s,
  and a second 5.0s tier was added — held from any screen, it immediately
  cancels an armed pulse and stops the beacon (the same effect as sending
  `QUIET`), added specifically because `BEACON` now transmits untethered
  and deserves a fast, unambiguous physical stop. Both thresholds fire
  independently during one continuous hold (e.g. holding through 5s on
  `BEACON` starts it at 2.5s, then immediately stops it at 5s). The 2.5s/
  5s retiming itself has not yet been physically re-confirmed the way the
  original 2.0s threshold was.
- **A boot splash screen was added** (`showSplash()`, `u8g2_font_9x15B_tf`
  title + device identity, held 1.5s before the menu appears). Compiles
  and boots correctly over serial; the actual visual result is unverified
  by me.
- **Fixed a real display bug, caught by an operator question:** the
  `SWEEP` screen was showing `active->name` (the currently-selected TX
  candidate) on its third line, which falsely implied Sweep uses that
  candidate's modem params. Sweep always uses the fixed MESH_OREGON tuple
  regardless of what's selected on the `CANDIDATE` screen (see
  `heltec-standalone-sweep.md`'s clarification on what "hardcoded" does
  and doesn't mean here) — the screen now reads `Fixed: MESH_OREGON`
  instead.
- **OLED layout is unverified by me** (I can't see the physical screen).
  Each screen draws a title plus up to three detail lines using the same
  `u8g2` calls the previously-confirmed display code already used, so risk
  is low, but the actual on-screen legibility/wrapping hasn't been
  eyeballed.
- **Correction:** the line originally here claimed `ENERGY_SWEEP` wasn't on
  Serial Control's remote allowlist. That was wrong — `SWEEP_START`/
  `SWEEP_CANCEL` were already implemented and hardware-verified
  (`serial_control.cpp`, `PROGRESS.md`'s Phase 9 Serial Control bullet). I
  had only read an earlier section of `PROGRESS.md` and hadn't actually
  opened `serial_control.cpp`. Confirmed live immediately after being
  corrected: `SWEEP_START` moved the Cardputer's Sweep to `RUNNING`
  (`WI=18;WN=221`), `SWEEP_CANCEL` ~0.3s later correctly reported
  `CANCELLED`. So the promised Heltec-vs-Cardputer side-by-side comparison
  no longer needs a physical key press at all — both can be driven
  remotely now. That comparison still hasn't actually been run; see
  `research/phase8-low-profile-harness-design.md`'s allowlist section and
  `CHANGELOG.md` for the correction and the `SD_RETRY` opcode added in the
  same pass.
- **More modes were invited** ("I am open to this device doing more modes
  both via serial and via the button interface") but not designed yet
  beyond what's here. The menu's `MenuScreen` enum and the
  action-per-screen dispatch in `runScreenAction()` are the extension
  point — a new screen is one enum value, one `drawMenu()` case, and one
  `runScreenAction()` case, reusing whatever guard pattern (`armed`/
  `beaconActive` checks) the new mode needs.
