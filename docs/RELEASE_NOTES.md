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

## v1.1.0-beta

**Focus Survey is now usable, as a beta.** Menu > Tools > Focus. Enter starts a
survey and Enter again cancels it. It parks on one frequency for two seconds,
records what it heard there, and returns to normal listening — you will see
`SURVEYING`, then `RESTORING`, then the result.

It picks the frequency for you: the strongest peak from your last Sweep, or
your home channel if you have not run one. So the useful order is Sweep first,
then Focus on what it found.

The plate shows the frequency, a dwell progress bar, and where the median, P90
and peak signal levels fell on a −120 to −40 dBm scale, plus how many passes
and samples it took and how long the radio was away from listening. If a
survey ever finishes without getting back to normal listening, the status
turns red and says `NO HOME` — that is the one result worth chasing.

**What it deliberately does not tell you.** There is no coverage label and no
confidence word. Whether a frequency was "sampled enough" depends on
thresholds we have not earned yet, and several plausible ways of deciding "is
something transmitting here" were measured on real hardware and thrown out —
including one that looked convincing until it was tested against a weak signal
instead of a loud one. Rather than show you a word we cannot stand behind,
Focus reports what it observed and stops. That is the main reason this is a
beta.

**The Activity page is rebuilt** and no longer disappears when a tool runs.
Previously starting a Sweep replaced it entirely; now it stays, showing a
30-second packet-rate graph and three cards: the strongest peak from your last
sweep, your last received packet with its signal quality, and how long the
radio was last away from listening. Up and down cycle three views — the
dashboard, the live Sweep page, and Waterfall — and **Enter, S and R all
control Sweep without leaving the page**, so you can leave a repeat sweep
running and watch it.

**Captures gains an inspector.** Press Enter on the Captures list to open it,
up and down to move through the ring, back to close. It shows signal strength,
SNR, frequency, length, modulation, and the first 32 bytes of the frame in hex.
Those bytes are shown exactly as received — nothing is decrypted, and if a
packet is longer than 32 bytes the header says so rather than pretending you
are seeing all of it.

**A note on that last one.** Showing packet contents on the device was
previously ruled out entirely. That has changed deliberately: the same bytes
already go to your SD card, so hiding them on screen protected nobody while
making the device worse for working out what you are looking at in the field.
Decryption is still not happening and still is not planned. Be aware that what
is on your screen belongs to whoever sent it.

---

## v1.0.8

**Fixes: USB status reporting could stop silently during long sessions.**
If you drive the device over USB with Serial Control, its `STATUS` line could
stop arriving partway through a long run — typically after several hours, once
the packet and error counters had grown a few digits. Nothing warned you: the
device simply stopped answering, which looks identical to a wedged receiver.
It is fixed, and there is now a test that catches the same class of problem
before it ships.

You are only affected if you use the USB control interface. Normal wardriving,
the on-device menu, SD logging and the WiFi web UI were never involved.

**No other change you will notice.** Groundwork for a future Focus Survey
feature ships in this build, but it is switched off: there is no menu entry
for it, no page, and nothing it can do on a normal image. It writes nothing to
your SD card. It is here so the release is one build rather than a
carried-forward patch, and it is deliberately not something to look for yet.

If you are curious what it will eventually do, the short version: park on one
frequency for a bounded time, record what was observed there, and restore
normal listening. What is not settled is how it should *describe* what it
observed — several plausible answers were measured on hardware and rejected,
which is why nothing is shown rather than something provisional.

---

## v1.0.7

**Tools and Analyze moved into the menu.** They were home-screen carousel
cards; now they're groups in the menu (backtick/ESC), alongside Profile and
System. Menu root is now **Profile / Analyze / Tools / System**. **Trace**
moved with them — it's now at **Menu > Tools > Trace** instead of its own
menu row (Enter still toggles it directly from the Radio page, unchanged).

Probe, Sweep, Cell, Meter, Waterfall, Scope, Captures, and Nodes work the
same as before once you're on them — this only changes how you get there:
through **Menu > Tools** or **Menu > Analyze** instead of paging to a home
screen card. Each row still shows its live status right in the menu
(SCANNING, COMPLETE, a live dBm reading, and so on) exactly like the old
home-screen cards did.

**Menu lists longer than 4 rows now scroll** (Analyze's 5 rows were the
first to need it) — the highlighted row always stays on screen, with a
small `^` or `v` mark on the top or bottom row whenever there's more above
or below.

**Left/right now moves between Probe/Sweep/Cell (or Meter/Waterfall/Scope/
Captures/Nodes) the same way up/down already did** — once you're on one of
these pages, `,`/`/` cycle to the next one in its group instead of leaving
to the menu, matching how the main Radio/Channel/GPS/System carousel
already works. The backtick/ESC key is still what takes you back to the
menu.

**Fixed:** closing the menu all the way out while on a Probe/Sweep/Cell/
Meter/Waterfall/Scope/Captures/Nodes page (say, after backing out of a
detour through System) now returns you to Radio instead of re-showing
that page.

**New home-screen page — Activity.** The main carousel is now **Radio /
Activity / Channel / GPS / System** (JUMP shortcuts: **1** Radio, **2**
Activity, **3** Channel, **4** GPS, **5** System; **6** unused). Activity
gives whichever bounded action is currently running (Probe, Sweep, Cell, or
Scope) the full panel with real live detail — progress and candidate count
for Probe, the same frequency position/occupancy/best-signal/lap numbers
Sweep and Cell's own cards show, Scope's tuned frequency. When nothing's
running, it lists each tool's real last result instead — hit count, peak
count and best frequency, best frequency and signal strength, last sample's
signal strength — rather than an empty screen. It's read-only: a status
mirror, not a way to start or cancel anything.

**Radio's own status banner is simpler now — just STANDBY.** With Activity
covering Probe/Sweep/Cell/Scope detail properly, Radio only shows
**STANDBY** when watch isn't actively listening (a manual pause, or any
bounded action currently running) — check Activity for which one and how
far along.

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
