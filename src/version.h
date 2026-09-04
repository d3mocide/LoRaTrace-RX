#pragma once
// LoRaTrace RX — firmware version. Single source of truth for the boot
// banner, the on-device UI, and release tagging.
//
// TWO different things live here, deliberately kept separate:
//
// 1. FIRMWARE_VERSION — the semantic version, bumped BY HAND. MAJOR.MINOR
//    tracks the build-order phase reached (docs/ROADMAP.md Versioning); PATCH is
//    for fixes that add no phase scope. This is a *statement* that a phase
//    was reached, so it must not auto-increment — a number that changes on
//    every build asserts nothing.
//
//    **Bump this when a phase lands, and before pushing the matching
//    `vX.Y.Z` tag.** CI enforces the tag/version match in release.yml, so a
//    mismatch fails the release rather than shipping a mislabelled binary.
//
// 2. FIRMWARE_BUILD_REV — the git revision, injected automatically at build
//    time by scripts/build_rev.py. This is what actually identifies a
//    build during hardware testing, where a dozen `dev-latest` binaries can
//    share one semantic version. Carries a "-dirty" suffix when built from
//    a modified working tree.

// 1.0.7: Tools and Analyze moved from main-carousel hub pages into real
// menu GROUPs (operator report: having both a home carousel with card-like
// hubs and a separate BACK-triggered menu read as "am I in the menu or
// not" in practice). Root menu is now Profile/Analyze/Tools/System; Trace
// moved from its own root row into Tools as a child row (it has no global
// hotkey the way P/S/C do, so it loses nothing by sitting one level
// deeper). This reverses part of 2026-09-04's "Tools/Analyze are ordinary
// carousel stops, no menu shortcut" call for those two specifically —
// Probe/Sweep/Cell's own "no duplicate root row" convention is unchanged,
// they're still one level under Tools/Analyze, not at the root themselves.
//
// The operator-facing main carousel shrank from six stops to four (Radio/
// Channel/GPS/System); JUMP_2/JUMP_3 now land on Channel/GPS instead of
// Tools/Analyze, and JUMP_5/JUMP_6 are unmapped. Probe/Sweep/Cell/Meter/
// Waterfall/Scope/Captures/Nodes are unchanged as real UiPage views (full-
// panel live rendering, own key handling) but are reached only through the
// menu now; UP/DOWN still cycle the sibling pages in the same group,
// PREV/NEXT/BACK all reopen the menu at its root instead of paging to a
// hub page that no longer exists. Enter-on-Radio still toggles Trace
// directly, unchanged — that shortcut isn't tied to Trace's root-menu
// position.
//
// Two follow-up fixes from the first bench pass, same day: folding
// Tools/Analyze into plain menu GROUP rows had silently dropped their
// per-row live status (SCANNING/COMPLETE/dBm/etc.) — the operator called
// this out as genuinely useful, lost from the old hub pages. Restored by
// extending menuEntryValue() (ui_pages.cpp) with a case per OPEN_* action,
// same state logic the deleted drawToolsPage()/drawAnalyzePage() used,
// just returning through the existing menu-row value column instead of a
// page-local array. Separately, Analyze's 5 rows at the menu's 24px
// row pitch overflowed under the footer hint text (System's own
// 4-row ceiling, SYSTEM_GROUP_ITEMS' comment, was never actually raised
// for a 5-row list) — drawMenuList() now scrolls any list past 4 rows,
// keeping the highlighted row in view and showing a small '^'/'v' cue on
// the top/bottom visible row when more sit outside the window. Generic
// over any list, not special-cased to Analyze, so a future group that
// grows past 4 rows gets this for free.
//
// Third same-day follow-up: on a Tools/Analyze sub-page (Probe/Sweep/Cell,
// Meter/Waterfall/Scope/Captures/Nodes), PREV/NEXT (left/right, ','/'/')
// now alias UP/DOWN and cycle the sibling pages in that same group instead
// of leaving to the menu — operator request, "helps these tool carousels
// work like the main carousel." Same alias direction the plain carousel
// already uses outside these pages (UP aliases PREV there). BACK is the
// sole "leave to the menu" key now.
//
// Fourth: BACK that closes the menu all the way out now returns to
// UiPage::RADIO if `page` is still a Tools/Analyze sub-page — previously
// it re-showed whichever Probe/Sweep/Cell/Meter/... page had been open
// before the menu was opened from there (operator report: closing out
// after browsing System landed back on a sub-tool page, not the main
// carousel). `page` isn't MenuState's to know about, so this is checked in
// ui_task.cpp right after menu.handle() returns, gated to a BACK that just
// closed the menu specifically -- SELECTing a Tools/Analyze row also
// closes the menu, but deliberately onto that exact page, and must stay
// untouched.
//
// Fifth: Radio's own status page now shows a banner while repeat Sweep or
// repeat Cell is running in the background, same two-line shape as its
// existing Probe banner (operator report: driving with a repeat scan
// active gave no visible sign of it on Radio, unlike Probe). Sweep gets
// "REPEATING" rather than Probe/Cell's "WATCH PAUSED" -- its home-channel
// capture window (v1.0.3) genuinely listens and captures packets between
// laps, so that claim would be false for it specifically; Cell has no such
// window and fully owns the radio during its scan, same as Probe.
//
// Sixth: a new main-carousel page, Activity, inserted at slot 2 (Radio,
// Activity, Channel, GPS, System — JUMP_2 now Activity, JUMP_3..5 shift to
// Channel/GPS/System, JUMP_6 unmapped) -- operator request, a follow-on to
// the fifth item above: Radio's own banner has to share space with rx/log/
// drop, so this gets the whole panel to mirror whichever bounded action
// (Probe/Sweep/Cell/Scope) is currently running, plus an explicit IDLE
// state when nothing is. Read-only, same as Radio's banner -- no SELECT/
// REPEAT handling of its own, so it cannot become a second way to start a
// scan (the exact "duplicate entry point" risk Probe/Sweep/Cell's own
// cards are kept off the root menu to avoid). Priority and wording
// (SCANNING/REPEATING/CAPTURING/WATCH PAUSED/CAPTURING ON HOME) match
// drawRadioPage()'s banner and menuEntryValue()'s OPEN_* cases exactly, so
// the same state reads the same word everywhere it's shown.
//
// Seventh, same day, bench feedback ("really bland and has a lot of dead
// space"): Activity now shows genuinely live detail per action instead of
// a bare state word -- Probe gets a real progress bar over its candidate
// count; Sweep/Cell reuse drawFreqBar()/drawSweepOccupancy()/
// drawCellBandBlocks()/statBlock() to show the exact same bin position,
// occupancy, best-signal, and lap numbers their own dedicated cards do
// (copied geometry, not reinvented, so it can't drift out of sync); Scope
// shows its tuned frequency. The idle case (nothing running) now lists all
// four tools' last result via menuEntryValue()'s own OPEN_* cases instead
// of a flat "no tool running" line, and reads STANDBY instead of IDLE when
// Trace is paused with nothing else active.
//
// Eighth: with Activity now covering it properly, drawRadioPage()'s own
// STANDBY/Probe/repeat-Sweep/repeat-Cell banner is gone -- operator
// request ("we can drop the activity state from the radio page now that
// activity has its own card"). Its one case Activity's idle branch didn't
// already cover (STANDBY, a manually-paused Trace with nothing else
// running) is why that branch now checks radioIsTracePaused() too (see
// seventh item above) -- otherwise removing the banner would have quietly
// dropped that signal everywhere, not just moved it.
//
// Ninth, same day, more bench feedback ("get rid of the top IDLE line
// completely its redundant... take a page out of analyzer's page and show
// some useful info from the last capture"): Activity's idle view drops its
// hero line entirely and moves the four rows up to fill the space. Each
// row's value is no longer menuEntryValue()'s OPEN_* state word (which
// read "IDLE" on all four most of the time, since they revert to it
// within RESULT_HOLD_MS of finishing) -- it's the real last-result numbers
// each tool's own card computes (hit count, peak count + best MHz, best
// MHz + dB, last Scope sample's dBm), same source data, just read directly
// instead of through the perishable state word. Shows plain "IDLE" (no
// summary, operator request: "instead of never run can it just say IDLE")
// for a tool genuinely never fired this boot. Net effect: this also removes the
// STANDBY-vs-IDLE distinction the eighth item above had just added --
// Trace-paused now has no on-screen indicator anywhere except
// Menu > Tools > Trace. Flagged, not silently dropped; worth revisiting if
// that turns out to matter in the field.
//
// Tenth: that flagged tradeoff turned out to matter -- STANDBY is back on
// Radio (operator request: "put that standby in the radio card again"),
// just the one line, not the rest of the old banner (Activity now owns
// the Probe/Sweep/Cell/Scope-specific detail properly). Checks
// radioIsTracePaused() directly, true whenever the radio isn't actively
// listening for any reason (manual pause or a bounded action owning it) --
// this line's job is only "is watch paused," not which of those it is.
//
// PATCH, not MINOR -- a UI reorganization within already-closed phase
// scope, no new capability. Not yet hardware-verified on real hardware;
// flag before calling this done.
#define FIRMWARE_VERSION "1.0.7"

// 1.0.6: correctness pass over what v1.0.5 shipped, from a code review and
// a whole-project audit (docs/research/2026-09-04-project-audit.md).
//
// Bumped for its own sake as much as the fixes: v1.0.5 was tagged and
// published, and then this work landed on top of it while version.h still
// read 1.0.5 -- so for a while a build reported a version whose released
// binary behaved differently. release.yml's guard compares a version string
// to a tag name and structurally cannot catch that, which is the audit's H1.
//
// Radio-ownership fixes (all in repeat-mode Sweep's new capture window):
// - energyActive now covers the window. It is the "energy subsystem owns
//   the radio" flag every other bounded action tests, and it was false for
//   ~70% of each repeat cycle: a P/C press during a window passed the
//   mutual-exclusion check, queued silently, and fired minutes later, and a
//   repeat-stop press never raised energyCancelRequested so the window ran
//   its full budget.
// - Captures from a window no lap ever reported (repeat stopped mid-window)
//   are discarded rather than left for the next sweep's snapshot to claim.
//   A later single-shot sweep, which runs no window at all, would otherwise
//   draw a green Waterfall mark and an "N PKTS" headline for packets it
//   never received.
// - The Pass-A margin is snapshotted once per sweep alongside the band
//   (radio_task.h's own "read at the start of each sweep, never mid-sweep"
//   rule) instead of read live per bin, so nudging the Margin slider mid-lap
//   can no longer judge one lap against two thresholds.
//
// Cross-core hardening (audit M1/M2), both latent rather than observed:
// - activeChannel/activeProfile are written under a spinlock and read back
//   under it. The struct is two floats plus three bytes written on Core 1
//   and read by value from six Core-0 sites; "cheap to return by value" was
//   never the same claim as "indivisible". Scope derives its acquisition
//   frequency from that struct, so a torn read parked it on a frequency no
//   profile ever used.
// - The per-sweep completion snapshot is published with an explicit
//   release fence, paired with an acquire in radioEnergySweepCount().
//   energyPeakBinMaskAtComplete is a plain array; volatile on the counter
//   ordered nothing about it. This path already produced one hardware-only
//   bug (v0.10.1's all-quiet Waterfall rows), so the ordering the comments
//   promised is now enforced rather than assumed.
//
// Menu itemCount static_asserts extended to ROOT_ITEMS, which is where the
// v0.8.9 bug they cite actually lived -- verified by reintroducing that bug
// and confirming the build fails.
//
// Settings-parser consolidation, same release (audit L1/M5): the four
// modules each carried a byte-identical copy of "trim, skip #comments,
// split on =, trim halves, reject empties" with only the per-key
// validation differing. That shared half is now config_line.h, and each
// module's apply...() moved into its header as a pure function -- which is
// what finally makes them host-testable (test_config_line/,
// test_settings_parse/, 20 new cases; 207 -> 227).
//
// That immediately exposed a real defect the missing tests had been
// hiding: Arduino's String::toInt() returns 0 for unparseable input, and 0
// is a *valid* value for two of these keys. `window_index=<garbage>`
// silently selected Capture: OFF and `idle_timeout_index=<garbage>`
// silently selected Idle dim: Off -- a corrupt line was honoured as a real
// setting instead of ignored. configParseLong() now requires the whole
// token to be digits. BRIGHTNESS_MIN/MAX/STEP also stopped being defined
// twice (display_settings.h owns them; ui_task_shared.h includes it), with
// a static_assert tying IDLE_TIMEOUT_OPTION_COUNT to the persisted index
// bound.
//
// PATCH, not MINOR -- correctness within Phase 9/10 scope, no new scope.
// Hardware-verified 2026-09-04 on real hardware: repeat Sweep refused all
// 25 PROBE_START attempts including 6 sampled during the capture window
// (the exact state v1.0.5 accepted them in), captures still landed
// (RXP 0->4), and the four settings files still load their real persisted
// values off the card (margin=300, brightness=40, idle_idx=1 -- three
// non-defaults, so the parser is genuinely reading the files rather than
// falling back).
// (Superseded by 1.0.7 above.)

// 1.0.5: Waterfall now shows packet captures, not just energy. Green
// (COL_GOOD) marks a bin where a real packet was demodulated and
// CRC-checked during that row's between-lap listen window; yellow
// (COL_WARN) stays "Pass A measured energy over the margin here". Two
// different claims, never blended -- a decoded packet is the stronger
// fact, and green routinely appears with no yellow under it because Pass
// A's per-bin glance is milliseconds against a 142-490ms packet. The
// headline reads "N PKTS" in green when energy found nothing but packets
// were still captured, instead of the flat "QUIET" that used to be
// actively false in exactly the case docs/STATUS.md's dwell-timing entry
// describes.
//
// Stored as its own WaterfallRow channel (capture_bin/capture_count), not
// packed into bins[]: that array is a quantized RSSI scale whose one spare
// value is already spent on "no sample", and reusing a plausible RSSI byte
// to mean "packet" is what its own comment warns against. A capture whose
// bin falls outside the row's swept range is recorded as none rather than
// clamped onto a real bin it didn't happen at. Fed by the same
// snapshot-at-completion discipline as the peak mask, for the same
// cross-core reason the v0.10.1 race fix exists.
// Verified on hardware over 45 consecutive rows: captureBin=34 throughout
// (910.525MHz in the US band, as predicted), 15 rows carrying 1-2 captures
// each, 21 packets total. Static footprint 6728 -> 6824 bytes of the
// design doc's 8192 ceiling (+96B, 1368 headroom); the session_log test's
// literal was updated deliberately to keep that canary meaningful.
// PATCH, not MINOR -- still Phase 9/10 scope.
// (Superseded by 1.0.6 above.)

// 1.0.4: the 1.0.3 capture window is now an operator setting, not a
// constant -- System > Tuning > Capture (Off/1s/2s/4s), persisted to
// /loratrace/capture.txt (capture_settings.h), read by the radio task
// through radioEnergySweepHomeListenMs(). It trades survey cadence against
// packet capture, which is an operator call rather than a tuning constant,
// so CLAUDE.md's "new operator-facing behavior gets an on-device toggle"
// rule applies. Default is 2s, the value the 1.0.3 A/B validated.
// Verified end-to-end on hardware (SD load -> radio task -> real capture):
// 8/17 packets on a short confirmation run, which pooled with 1.0.3's own
// 22/27 gives **30/44 = 68% (95% CI 54-82%)** -- the better estimate; the
// single-run 81.5% was optimistic at n=27. Still comfortably past 50%.
//
// Also makes a whole bug class a build error: every nested menu group's
// hand-written itemCount is now static_assert'd against its array's real
// length. A stale count silently hides the extra rows and nothing at
// runtime notices -- exactly what shipped in v0.8.9 when Region became
// System's 4th row while the count still said 3, caught only by an
// operator not seeing it on hardware. Verified by deliberately breaking
// the Tuning count and confirming the build fails.
// PATCH, not MINOR -- still Phase 9 scope.
// (Superseded by 1.0.5 above.)

// 1.0.3: repeat-mode Sweep now timeshares the radio with the home channel
// instead of monopolizing it -- after each completed lap it parks on home
// with RX armed for the capture window (2000ms default) and services real
// packets through HOME_LISTEN's own readDetectionLocked()/enqueueDetection()
// path, so a packet caught mid-sweep is indistinguishable downstream from
// one Trace caught while idle. Single-shot Sweep is deliberately unchanged.
// Measured on real traffic against the operator's pyMC_Repeater as ground
// truth, two back-to-back 4-minute windows (docs/hardware-results/private/
// capture-rate-*): **0/42 packets captured (0.0%) with the window disabled,
// 22/27 (81.5%) with it at 2000ms**, zero CRC errors, lap time unchanged at
// ~833ms. That 0% baseline is the real headline -- docs/STATUS.md had
// recorded a ~15x drop in Trace's catch rate during sweeps, and measured
// directly it is closer to total blindness.
//
// Also reverts 1.0.2's ENERGY_SWEEP_SAMPLES_PER_BIN 4 -> 34. That change
// was never justified by real-traffic data (the A/B for it came back
// statistically indistinguishable) and the model says it was the wrong
// lever entirely: a packet's airtime (142-490ms measured, median 244ms) is
// one to two orders of magnitude longer than any per-bin dwell a full-band
// sweep can offer, and *decoding* a packet needs the receiver parked on its
// frequency for the whole airtime -- which no 85-bin sweep can do for any
// single bin at any dwell width. Capture is bounded by share of wall-clock
// time on the channel, not by dwell, which is why timesharing works and
// tuning the sample count could not.
// PATCH, not MINOR -- still Phase 9 scope, same convention as 1.0.1/1.0.2.
// (Superseded by 1.0.4 above.)

// 1.0.2: performEnergySweep()'s per-bin retune is no longer a full
// radio.begin() (RadioLib -> modSetup(): hardware reset, chip re-detect,
// TCXO restart, full config reload -- everything, even though SF/BW/CR/
// sync never change across a sweep, only frequency does). Now: one real
// begin() at bin 0 (and again immediately after any bin that ran a Pass-B
// CAD attempt, which leaves the radio on that candidate's own SF/BW/sync),
// standby()+setFrequency()+startReceive() -- three single-SPI-command
// calls -- every other bin. RadioLib's own setFrequency() only re-runs
// image calibration past a 20MHz jump (RADIOLIB_SX126X_CAL_IMG_FREQ_TRIG_
// MHZ); this sweep's 250kHz step never crosses that.
// Hardware-confirmed same day, isolated from the Pass-B/margin confound
// that complicated the first attempt at this measurement: with System >
// Tuning > Margin held at the calibrated 35.0dB default (zero Pass-A
// peaks either side, so zero Pass-B CAD time muddying the numbers), four
// back-to-back US-region (85-bin) sweeps before vs. after --
// 3471/3435/3475/3472ms (avg 3463ms, ~40.7ms/bin) vs.
// 834/866/833/866ms (avg 850ms, ~10.0ms/bin) -- a real, repeatable ~4.1x
// per-bin speedup, not an estimate.
// Prompted by the same dwell-timing investigation 1.0.1 references: a
// lighter retune doesn't fix the RTL-SDR-confirmed dwell-vs-burst-timing
// miss on its own, but it recovers real time that can go toward more
// samples per bin -- decided during this session, sizing left for a
// follow-up once the freed budget is spent on purpose rather than by
// default. Separately, this same session found Pass-B CAD's own bounded
// receive-on-hit window (DISCOVERY_RX_WINDOW_MS, 2.5s) now dominates total
// sweep time whenever the margin is sensitive enough to find several
// peaks -- up to ~20s across the 8-peaks-per-sweep cap, dwarfing the
// per-bin cost this entry fixes. Evaluated and left as-is (operator
// decision, same session) -- tracked in docs/STATUS.md, not fixed here.
// PATCH, not MINOR -- an enhancement within Phase 9's already-closed
// scope, same convention as 1.0.1 above.
// (Superseded by 1.0.3 above.)

// 1.0.1: Sweep's Pass-A peak margin (energy_observation.h's
// ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10, a single-quiet-room bench
// calibration) is now operator-adjustable at System > Tuning > Margin
// (15.0-50.0dB, 5.0dB steps), persisted to /loratrace/sweep_margin.txt.
// Prompted by an operator field report (real deck-range readings of
// -55 to -72dBm) after docs/STATUS.md's 2026-09-03 "Sweep silence"
// investigation had already cleared the margin as the cause at 6ft/59dB
// clearance -- that investigation's own conclusion flagged the margin as
// still worth making adjustable "if a future need justifies the menu-
// toggle work," and a weaker/more distant real signal is exactly that:
// its clearance over the floor can drop under the 35dB default even with
// a clean receiver SNR. PATCH, not MINOR -- an enhancement within Phase
// 9's already-closed scope, not new phase scope, same convention as
// 0.10.2/0.10.3 above. Dwell timing (that same investigation's actual
// root cause for the specific case it tested) is unchanged by this --
// tracked separately, not fixed here.
// (Superseded by 1.0.2 above.)

// 1.0.0: promotion decided 2026-09-03 (same day Phase 10 closed) --
// ROADMAP.md's own documented v1.0.x gate was Phase 10 (Field Analyzer)
// closing, and only that: "v1.0.x is not tagged until Field Analyzer's own
// exit criteria... are also closed" (Phase 10's own section), matching the
// original "all four profiles + UI stable" framing this doc has used
// since the 2026-08-25 renumbering. That's done -- all five exit criteria
// hardware-confirmed at v0.10.1, plus the v0.10.2/v0.10.3 UI polish above.
// Phase 11 (Cell) was never a v1.0 gate -- added out of sequence, appended
// after Phase 10 rather than inserted into it, outside the original
// four-profile scope. Two Cell items stay open post-v1.0 (a real
// cell-band RSSI rise near a known tower, cell.csv/session.csv's Cell
// columns writing correctly) -- known, tracked gaps (docs/STATUS.md), not
// blockers by the doc's own gate, an explicit operator call rather than a
// silent omission.
// (Superseded by 1.0.1 above.)

// 0.10.3: two more same-day enhancements, both workshopped in
// docs/research/analyzer-preview.html before reaching real hardware, same
// as 0.10.2 below:
// - Waterfall's frequency axis merged into the plot box's own bottom
//   border (operator request: "can the bottom of the waterfall chart
//   become the marker for the frequency?") -- drawWaterfallFreqAxis() no
//   longer draws its own hline, removing a redundant near-parallel line
//   and the gap before it; PLOT_H grew again (55 -> 60) with the
//   reclaimed space.
// - Meter gained a real bar gauge (drawMeterBar(), fill not
//   drawFreqBar()'s position marker -- signal strength is a quantity,
//   frequency is a location), an SNR line (CaptureSummary.snr_db, real
//   data this page had access to and never showed), and a right-column
//   SF/BW/CR block (same source, same reasoning) -- all three watch-
//   sourced only, since Scope never demodulates a signal and genuinely
//   has neither SNR nor channel params to report. Bar range widened
//   -30 -> 0dBm same day after a real -16dBm reading clipped flat
//   against the original ceiling (deliberately not shared with
//   drawScopePage()'s own -120/-30, which exists for a different reason
//   -- trace-height comparability, not clipping avoidance).
// Hardware-confirmed same day. PATCH, not MINOR -- same reasoning as
// 0.10.2.
// (Superseded by 1.0.0 above, same day.)

// 0.10.2: Waterfall enhancement, same day as 0.10.1 (below) -- a frequency
// axis under the plot (lo/center/hi MHz + four reference ticks,
// drawWaterfallFreqAxis()) and Enter now starts/stops repeat Sweep
// directly from the Waterfall page (WATERFALL_SWEEP_REPEAT_TOGGLE), with a
// "SCANNING" badge on the headline row while active. The plot box itself
// moved twice in the same session (64px shipped -> 45px to fit the axis ->
// 55px once the box's top edge was pulled up to close a ~10px gap under
// the meta line) -- both operator-driven layout passes, workshopped in
// docs/research/analyzer-preview.html before either reached real hardware,
// same tool that already caught one real footer-collision bug for this
// page (0.10.0's own history). Hardware-confirmed same day: "waterfall
// works amazing." PATCH, not MINOR -- enhancement within Phase 10's
// already-closed scope, not new phase scope.
// (Superseded by 0.10.3 above, same day.)

// 0.10.1: all five Phase 10 exit criteria are now closed, same day as
// 0.10.0 (below). The worst-case UI/radio run (WiFi on, Sweep repeat mode,
// Waterfall open, 60 real minutes) came back clean -- a background Serial
// Control watch logged 231 STATUS polls, zero dropped/unanswered, zero
// task_wdt/Guru Meditation signatures -- and the operator confirmed
// readability both in direct window sunlight and indoors. That same
// worst-case run surfaced a real bug: Pass A found 50 energy peaks
// (STATUS's own PBA field) over the hour, yet Waterfall showed nothing.
// Root cause: analyzer_state.cpp's analyzerNoteSweepComplete() (Core 0)
// read radio_task.cpp's live per-sweep peak-bin mask, but in repeat mode
// radio_task's own do-while loops straight into the next lap with no
// delay, and that lap's first line resets the same mask -- Core 0's
// ~100ms poll cadence almost always lost that race, so every repeat-mode
// Waterfall row read an already-cleared mask. energy.csv itself was never
// affected (a separate, queue-based path). Fixed with a second, stable
// snapshot buffer taken atomically at sweep completion
// (radioEnergyPeakBinSetAtLastComplete(), radio_task.h/.cpp) that
// analyzerNoteSweepComplete() now reads instead; the Sweep page's own
// live occupancy ticks (drawSweepOccupancy(), ui_pages.cpp) are untouched
// on purpose. PATCH, not MINOR -- a fix within Phase 10's already-claimed
// scope, not new scope, same convention v0.8.6-v0.8.9 established.
// (Superseded by 0.10.2 above, same day.)

// 0.10.0: Phase 10 (Field Analyzer) reached MINOR status -- not all five
// ROADMAP.md exit criteria were closed yet (the 1-hour worst-case soak and
// outdoor/minimum-brightness readability checks were still open, both
// needing an operator physically driving the keyboard -- both closed the
// same day, see 0.10.1 above), but MINOR tracks "the phase reached", same
// convention v0.8.x/v0.9.0 already established, and enough of Phase 10 was
// real and hardware-verified to warrant it: the full data layer
// (waterfall.h/scope_trace.h/capture_history.h/node_roster.h, 202/202
// host tests), the radio-owned SCOPE_ACQUIRE state (radio_task.cpp), and
// the on-device UI -- Meter/Waterfall/Scope/Captures/Nodes plus a real
// Analyze hub, all confirmed booting and running on real hardware. A
// second hub (Tools, gating Probe/Sweep/Cell) was added the same session
// at the operator's request -- real scope beyond docs/research/
// LoRaTrace-Phases-7-10-Design.md's own §8, not a deviation from it; see
// docs/ROADMAP.md's Phase 10 entry. Measured: the four analyzer structures
// commit 6,728 bytes of static storage against the design doc's 8,192-byte
// ceiling (§8.3) -- 82.1% used, 1,464 bytes headroom -- computed directly
// from sizeof(), not estimated; the real linked-firmware RAM delta since
// this work began (+6,472 bytes) tracks within compiler-alignment noise of
// that number. Confirmed a second way, 2026-09-03: a real 29-minute
// session.csv (run0006) reports analyzer_static_bytes=6728 on every row,
// sd=ok throughout, zero row/queue/bus drops, heap settling once then flat
// for the run.

// 0.8.8: Cell's frequency bar now labels the FCC's own 869-894MHz A/B
// channel-block split (47 CFR S 22.905, cell_plan.h's CELL_BAND_BLOCKS) --
// regulatory block letter only, never a carrier name. Still Phase 11 scope
// (docs/ROADMAP.md), so PATCH again, same reasoning as the 0.8.6/0.8.7
// bumps below.

// 0.8.7: Sweep's R-key repeat toggle is now page-gated (Sweep card only,
// was global) and Cell gained the same repeat mode; Probe still has none
// (operator decision: "Repeat only on the Sweeps"). Still Phase 11 scope
// (docs/ROADMAP.md), so PATCH again, same reasoning as the 0.8.6 bump below.

// 0.8.6: Cell (869-894MHz RSSI presence sweep) added as a bounded
// radio-owned action alongside Probe/Sweep — see docs/ROADMAP.md's
// out-of-sequence "Phase 11" entry for why this is a PATCH bump, not a MINOR
// one: it is not the next build-order phase (Phase 9/ENERGY_SWEEP is still
// in progress), so bumping MINOR would misrepresent Phase 9/10 as reached.

// Fallback for builds that bypass the PlatformIO extra_script (e.g. the
// host-native test env, or an IDE indexer). Never seen on a real firmware
// build — if this string reaches a device, the script didn't run.
#ifndef FIRMWARE_BUILD_REV
#define FIRMWARE_BUILD_REV "unknown"
#endif
