# Workstream 17 — Pass A reads a floor ~14 dB low as shipped

**Build:** bench image, `V=1.0.7;BENCH=1`. **Date:** 2026-09-05.
**Link:** Workstream 12's porch fixture at its stronger setting, matched whips
vertical, transmitter on `MESH_OREGON` (918.5 MHz -> Sweep bin 66 of 85).
**Raw:** `private/ws17-retune-probe.log`, `private/ws17-check*.jsonl`.

Entry measurement for the candidate workstream. `src/version.h` records an
11 dB under-read on Cell caused by sampling RSSI before the AGC settles after
a light retune, and states plainly that "Whether Sweep has the same under-read
is unknown". This measures it.

Each row: two silent laps and three laps with the transmitter radiating,
median of `BENCH_SWEEP_FLOOR` for bin 66.

| Pass A configuration | silent floor | source on | delta |
|---|---|---|---|
| **LIGHT + 0 ms settle (shipped)** | **-115.0 dBm** | -114.8 | +0.2 dB |
| LIGHT + 5 ms settle | -101.5 dBm | -102.5 | -1.0 dB |
| LIGHT + 20 ms settle | -101.7 dBm | -98.2 | +3.5 dB |
| FULL `begin()` per bin | -112.3 dBm | -112.5 | -0.2 dB |

## Established

**Pass A as shipped reads the noise floor about 14 dB below a settled
receiver.** Adding a 5 ms settle moves the silent floor from -115.0 to
-101.5 dBm, and 20 ms holds it at -101.7. Focus independently measures the
ambient floor at this frequency as -99 to -101 dBm across hundreds of passes,
which is where the settled readings land and is 14 dB above the shipped one.

That answers `version.h`'s open question: Sweep has the same defect Cell had,
and larger. It matters because Pass A's peak decision compares a bin against a
noise floor plus a margin, and both terms are being computed from under-read
samples. `ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10` was calibrated against
those same under-read values, so the margin is not obviously wrong — but it is
calibrated on a scale that does not correspond to real dBm, which makes any
absolute reasoning about Sweep sensitivity unsound until this is settled.

## Not established, and not to be inferred from this

**Whether Pass A can flag a bin carrying traffic remains open.** The
source-present column is not a clean measurement and should not be read as
one. Even at 20 ms settle the source produced only +3.5 dB, when Focus reads
the same transmitter at -69 to -75 dBm against a -100 floor. Candidate
explanations, none yet tested: the source is on air for only part of each
lap's 3 ms visit to bin 66, so a median across three laps washes it out; the
firing pipeline's ~1.5 s bridge latency makes "source on during this specific
3 ms" hard to guarantee; and the `FULL begin()` row is internally odd, reading
a *lower* floor (-112.3) than the settled light path despite skipping the
settle only because a full begin already takes longer.

Two earlier attempts at this measurement were invalid for the same reason and
are recorded so the next one does not repeat them: firing only for a lap's
duration radiates entirely after the lap ends, and reads as a silent band in
every arm.

## Next

- Establish the floor result properly: more laps, both link levels, and a
  settle sweep finer than 0/5/20 ms, to find where the reading stabilises.
- Only then ask the detection question, with the source's presence during the
  bin visit *verified* rather than assumed — which needs a device-side
  timestamp, not a host-side one.
- Decide separately whether the margin constant should be recalibrated against
  settled readings, or whether Pass A should settle before sampling. Those are
  different fixes with different costs to lap time.
