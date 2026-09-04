# Release notes

Operator-facing notes, one section per released version, newest first.
`release.yml` publishes the section matching a `vX.Y.Z` tag as that
release's body and **fails the release if the section is missing**, the same
way it already fails a tag whose `src/version.h` disagrees with it.

Three files carry release history and they are not interchangeable:

| File | Audience | Answers |
|---|---|---|
| `src/version.h` | maintainers | *why* a change was made, what was measured, what was rejected |
| `CHANGELOG.md` | maintainers | terse running log, newest first, by date |
| **this file** | **operators** | *what changed for someone flashing and using the device* |

Write these for someone holding the hardware. Name the on-device menu path
when a setting moves or appears, say what behaviour they will notice, and
say plainly when something is unverified. Skip internal identifiers, file
names and refactors that change nothing observable — those belong in the two
files above.

Versions before `v1.0.6` predate this file; their history is in
`CHANGELOG.md` and `src/version.h`.

---

## v1.0.6

**Sweep now captures packets while it scans.** Previously, running Sweep in
repeat mode meant the receiver heard essentially nothing — measured at 0 of
42 real packets during a 4-minute run. Repeat Sweep now pauses on your home
channel between laps to actually receive, and captured packets appear in
`detections.csv`, the Captures card and the Nodes roster exactly as if Trace
had caught them. Measured 30 of 44 packets (68%) over two runs against a
real mesh.

**New setting — System > Tuning > Capture** (`Off` / `1s` / `2s` / `4s`,
default `2s`). This is the trade: a longer window captures more packets, a
shorter one gets you round the band faster. At the 2s default a full-band
survey takes about 2.9s instead of 0.9s. Set it to `Off` for the old
scan-only behaviour. Saved to `/loratrace/capture.txt`.

**Waterfall now distinguishes what it heard from what it decoded.** Green
marks a bin where a real packet was demodulated and CRC-checked; yellow
remains an energy reading above the margin. Green often appears with no
yellow beneath it — that is correct, not a glitch: Sweep's per-bin look is
milliseconds against a packet lasting 142-490ms, so it frequently misses
traffic the receiver then decodes cleanly. The header reads `N PKTS` in
green when packets were captured but no energy peak was found, instead of
the flat `QUIET` it used to show while traffic was actively being recorded.

**Fixes**

- Starting a Probe or Cell scan during a repeat Sweep is now correctly
  refused instead of being silently queued and firing minutes later.
  Stopping repeat Sweep also takes effect immediately rather than waiting
  out the capture window.
- The Waterfall could show a green packet mark on a sweep that had received
  nothing, if repeat mode was stopped mid-window. Those readings are now
  discarded.
- Adjusting System > Tuning > Margin mid-sweep no longer judges one lap
  against two different thresholds.
- A corrupt or hand-edited `capture.txt` or `display.txt` could silently
  select "Off" for the capture window or idle-dim, because unparseable text
  was being read as `0`. Bad values are now ignored and the previous setting
  is kept.
- Sweep's radio ownership, the channel shown on status pages, and the data
  behind the Waterfall are all hardened against cross-core races. No
  symptom was reported for these; they were found by audit.

**Known issues**

- `energy.csv`'s absolute RSSI values read about 2.4 dB lower than they did
  before `v1.0.2`. Peak detection and the 35 dB margin are unaffected — only
  comparisons of raw RSSI across that boundary. See
  `docs/LOG_GUIDE.md`.
- The GitHub Pages web flasher previously failed to update when a release
  was published. A fix is included but is unverified until a release is
  actually published with it in place.
