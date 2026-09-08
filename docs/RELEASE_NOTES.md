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

## v1.1.2

A follow-up to v1.1.1 that finishes the audit it came from. Nothing here
changes how you use the device day to day; most of it is the receiver being
harder to knock off the air, and the logs being honest about it.

**Re-export anything you took with "NODES (SAFE)" on v1.1.1.** Fuzzing found
two ways that export could produce a file that was not actually
spreadsheet-safe: certain unusual rows fell through a path that skipped the
protection entirely, and adding quotes around a name that already contained a
quote broke the row so later columns stopped being read as the columns they
were. Neither could be triggered by files this device writes — its own writer
never produces those shapes — but if you exported from anything else, or want
to be sure, take the export again on this build. A row this device now cannot
safely convert is refused outright rather than exported looking fine.

**The radio can no longer be held off the air by the screen.** Scope waited on
a display lock without any limit, and while holding the radio's own bus. On a
busy card that could stall reception. Scope and Cell now also stop and return
to listening if they exceed their time budget, instead of running as long as
the work happens to take.

**Focus stops pretending about sample timing.** If the card or the screen
delayed a sample past its slot, Focus used to take it late and count it
anyway, so a row claimed an even spacing the measurement never had. Late slots
are now skipped and counted in a new `skipped_samples` column in `focus.csv`.
A non-zero value means that pass ran under contention — not that anything was
wrong with the signal.

**An SD card dropping out is now recorded, not just implied.** A health row
cannot be written while the card is missing, so an outage was previously only
a gap between rows. When the card comes back, the device writes a
`reason=sd_recovered` row saying how long it was gone, and `session.csv` gains
`sd_outages` and `sd_last_outage_ms`.

**Pressing two action keys quickly now refuses the second.** Before, both
could be accepted while neither had started, and they would run one after the
other — so an action you thought you had cancelled ran a minute later. Related:
a profile switch that could not get the card bus used to be dropped silently,
leaving the radio on the old profile with nothing saying so; it is retried
now.

**Bench fixture users:** the Heltec transmitter's network bridge now requires a
token before it accepts any command. It is printed on that fixture's USB
console when the bridge starts, and the bench scripts take it as
`--bridge-token`. A fixture connected by USB cable needs nothing new.
Sessions expire after 20 minutes and when the client disconnects, and a new
connection quiets the transmitter rather than inheriting whatever the last one
armed.

**Known issues, unchanged from v1.1.1.** The AP has no login beyond the Wi-Fi
key and no encryption. The full-card and failing-card paths are handled in
code but still have not been forced on real hardware. Cell's field validation
against a known tower is still deferred.

---

## v1.1.1

**Your Wi-Fi password changes with this update, and you cannot get the new one
over Wi-Fi.** Every build before this one used the same password,
`loratrace123`, baked into the firmware — which meant anyone who had ever seen
any LoRaTrace build could join your AP and download your captures. Each device
now generates its own key the first time the AP starts.

Read it on the device: **System > Connectivity > Wi-Fi Key**. That opens a
screen showing the network name and the key, and it stays up until you press
Enter or `` ` `` — it is not a toast, because twelve characters is more than
anyone can copy in a second and a half. Write it down once; it survives
reboots. To change it, delete `/loratrace/wifi.txt` from the card and enable
the AP again. If the device could not write to the card it will say
**"not saved - changes on reboot"** on that screen, and it means it.

The key is deliberately not available over serial, over the web page, or in
any export or log. That screen is the only place it exists.

**Cell is worth using now. Everything it recorded before is not.** Three
separate faults were stacked on top of each other, and each one hid the next:

- it never actually tuned to the frequency it was recording against, so every
  reading belonged to the previous one;
- roughly a third of every lap never reached the card, while the health log
  reported no drops at all;
- and it read signal strength before the receiver had settled, so about two
  thirds of bins came back at a floor value belonging to no frequency.

All three are fixed. A lap now writes all 101 bins and reads a real level on
essentially every one. **Delete or ignore any Cell data captured before this
release — including anything that showed a frequency as quiet.** A "quiet" bin
was usually just an unsettled reading; the same band now reads about 22 dB
louder. Laps take about as long as before.

Cell is still an optional narrowband signal-strength survey. A strong bin is
not a tower, a distance, or a calibrated power measurement.

**Two files you could never download are downloadable.** `cell.csv` and
`focus.csv` were missing from the web page and from the device's own list of
allowed files, so the only way to get them was to pull the SD card. They are in
the run list now.

**Each run folder gets a `manifest.txt`.** It records which firmware built it,
the exact radio settings in use, the region, and one block per power-on — so a
folder you copy off the card months later can still say what the device was
when it heard all that. It also states two things people get wrong: the
millisecond columns are device uptime, not clock time, and a position is where
*your receiver* was, never where a transmitter was. It downloads with the CSVs.

**A "NODES (SAFE)" download sits beside the normal one.** Node names arrive over
the air, and a name like `=1+1` is treated as a formula by every spreadsheet.
The normal file keeps exactly what was received; the safe one is for opening in
a spreadsheet. Negative signal values are left alone.

**If a settings page was already open in a browser, reload it.** Saving now
requires a token the page picks up when it loads, so a tab left open from
before will answer "reload the page" instead of saving. This stops another
website you happen to be visiting from quietly changing your radio settings
while you are connected to the AP.

**The settings page now shows saved and running values separately.** Before, it
always showed what the radio booted with, so an unrebooted save looked like it
had not happened — and editing that form silently reverted it. The form now
edits what is on the card and tells you when the radio is still running
something else.

**Timestamps and positions are attributed more carefully.** A packet is tagged
with the GPS fix that was current when it was *received*, not when it was
written to the card — those differ when the card is busy and you are moving.
UTC now comes only from a complete GPS date-and-time pair and stops being
reported when the GPS stops, instead of a frozen clock that still looks
authoritative.

**New columns, and one to stop over-trusting.** `classification` in
`detections.csv` has always meant *what the radio was listening for*, not what
the packet was — but it reads the other way, especially under Reticulum or
General Exploration, which listen on the Meshtastic channel. Three columns now
carry the honest answer: `protocol_candidate`, `parse_status` and
`auth_status`. `auth_status` always reads `unauthenticated`, and that is the
point — nothing this firmware decodes proves who sent it. Treat every node
name, ID and key as a claim.

`session.csv` gains `home` (whether the radio was actually listening — a quiet
run with `home=down` is not evidence the band was quiet), separate reception
error counts, SD write-fault counters, and a per-boot ID.

**Known issues.** The AP still has no login beyond the Wi-Fi key and no
encryption: treat anyone who joins as having full access. The full-card and
failing-card paths are handled in code but have not been forced on real
hardware. Cell's field validation against a known tower is still deferred.

---

## v1.1.0

**The whole home screen is rebuilt.** Every card now carries more than one
screen. Left and right still move between cards; **up and down move within
one**, and the dots at the bottom centre tell you how many screens a card has
and which one you are on. Enter runs whatever that screen is about.

| Card | Press up/down for |
|---|---|
| Radio | Meter, Scope |
| Activity | Sweep, Waterfall, Focus |
| Channel | Probe, Captures, Nodes |
| GPS | Cell |
| System | — |

Because every tool now lives on the card it belongs to, the **Tools and Analyze
menus are gone**. The menu is Profile, Trace, System. Nothing was removed — the
same pages are one press away from the card that owns them. Digits 1–5 still
jump straight to a card, and P, S, C still start Probe, Sweep and Cell from
anywhere.

**Focus is finished, and it now tells you how much you have looked.** Its result
carries a coverage word: `sampled` after one survey of a bin, `repeated` after
three. Survey a different frequency and it starts over, because time spent on
one bin says nothing about another.

Coverage means **how much looking happened, and nothing else**. It is not signal
strength and not a guess about whether the frequency is empty. Focus still does
not tell you that something transmitted — several ways of deciding that were
measured on real hardware and rejected, most recently one that worked well
enough in general but got *worse* on a busy band, which is exactly where you
would want to trust it. It reports what it observed and stops.

**Cards you already knew, rebuilt:**

- **Radio** is the receive chain as six numbers — heard, CRC, bus misses, then
  queued, logged, dropped. A loss figure is green at zero and red otherwise, so
  a leak is visible without comparing anything.
- **Channel** shows the whole band with your tuned frequency marked, green ticks
  where packets actually decoded and amber where the last Sweep found energy. If
  the traffic is somewhere you are not, you can now see that.
- **GPS** shows a 60-second trace of satellites used, so a fix that keeps
  dropping out under trees is visible while it happens. A new **TAGGED** card
  counts detections that got a real position — the number that says how much of
  a run is actually mappable.
- **System** plots free memory and battery over 30 minutes, and reports battery
  as a **measured** discharge rate over a stated window rather than a guess at
  hours remaining.
- **Sweep, Cell and Probe** now lead with what they found rather than a status
  word. Probe moved to Channel, directly after the card, since it answers "should
  I be on a different channel".

**WiFi moved to a dot in the top bar**, beside the GPS and heap dots. The
firmware version shows in the header on the System card.

**Reliability.** An 8-hour run of continuous sweeps completed 4,112 laps with
zero failures, no dropped rows, and memory that stopped changing after startup
and stayed flat for the following ten hours. Two problems it exposed are fixed
here: the display task was running much closer to its memory limit than intended,
and a diagnostic counter added the same week overflowed after about seven hours.

**Known limitations, stated plainly:**

- **Field validation has not been done.** This release was tested on the bench
  and over long unattended runs, not on a drive. It is deferred deliberately, not
  forgotten.
- Sweep's follow-up stage spent nearly four of the eight soak hours checking
  candidate channels and promoted nothing. That is being investigated separately;
  if you care more about hearing packets than about finding new channels, run
  Sweep less.
- `focus.csv` records the coverage word next to the raw pass count and observed
  time it came from. If they ever disagree with each other, trust the numbers.

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
