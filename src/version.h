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
#define FIRMWARE_VERSION "1.0.5"

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
// with RX armed for ENERGY_SWEEP_HOME_LISTEN_MS (2000ms) and services real
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
