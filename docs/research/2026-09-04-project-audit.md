# LoRaTrace RX — project audit, 2026-09-04

**Scope:** whole-project review and audit. Findings only — nothing in this
document was fixed as part of writing it (the one exception is called out
explicitly in §6, work that predates the audit instruction).

> **Update, later the same day:** the findings below were subsequently
> acted on in `v1.0.6`. The audit text is deliberately left as written, as
> a point-in-time record; the Status column in §1 and the notes in §9 say
> what was done. Everything fixed is built and host-tested but **not yet
> hardware-verified** — the Cardputer was disconnected.

**Commit audited:** `1df46bc` ("Add configurable sweep capture and waterfall
status") = `origin/main`, plus the uncommitted working tree on top of it.
**Tags present:** `v1.0.0`, `v1.0.5`. **Tests:** 207/207 host cases pass;
`cardputer-adv` and `cardputer-adv-bench` both build clean.

**Method:** static reading of `src/` (16.5k lines across 75 files), the three
workflows, the test tree, and the live GitHub Actions history via the REST
API. Hardware was available for the first part of the session but was
unplugged before the audit, so nothing here is a fresh hardware measurement —
where a number is quoted it comes from this session's earlier measured runs
or from `docs/STATUS.md`.

**Confidence labelling:** every finding below states its evidence. Items I
could not confirm are marked *unverified* rather than asserted.

---

## 1. Summary

| # | Severity | Area | Finding | Status |
|---|---|---|---|---|
| H1 | **High** | Release integrity | `version.h` says `1.0.5`, but `v1.0.5` is already tagged and published with *different* code | **Fixed** v1.0.6 |
| H2 | **High** | CI | `release.yml`'s manual dispatch can overwrite a published release's assets with a different build, and the guard cannot currently catch it | **Fixed** |
| M1 | Medium | Concurrency | `activeChannel` is a multi-word struct read cross-core with no atomicity | **Fixed** |
| M2 | Medium | Concurrency | Peak-mask arrays shared cross-core are non-volatile; snapshot ordering is unbarriered | **Fixed** |
| **M6** | Medium | Measurement | Sweep's light retune reads the noise floor **−2.40dB** low (reproduced in 5/5 runs). Whether the under-read *grows with signal strength* is **unproven** — three attempts, three contradictory answers, control failed to reproduce | Floor offset confirmed; signal-dependent claim **retracted** |
| M3 | Medium | CI | Pages deploy fails on every non-`main` ref (the reported tag failure) | Fix applied, **unverified** |
| M4 | Medium | CI | `Download dev-latest assets` hard-fails if that release is missing | **Revised** — see §9 |
| M5 | Medium | Testing | Four hand-rolled settings parsers, zero host tests between them | **Fixed** — and found a real bug, see §9 |
| L1 | Low | Structure | ~400 lines of near-duplicate settings modules | **Fixed** |
| L2 | Low | Performance | Cell sweep still pays the full `begin()`-per-bin cost Sweep shed | **Tried and reverted** — see §10 |
| L3 | Low | Performance | Pass-B's receive window now competes with the capture window | Open |
| L4 | Low | Structure | `radio_task.cpp` is 2,319 lines and is where every known race has originated | Open (observation) |
| L5 | Low | Testing | `ui_pages.cpp` pure helpers are testable but untested; one duplicates library logic | Open |
| L6 | Low | Docs | `CLAUDE.md`'s layout omits 18 real modules | **Fixed** |

Nothing found is memory-unsafe, and nothing violates the RX-only rule.

---

## 2. High severity

### H1 — `version.h` no longer identifies the binary it claims to be

`src/version.h` currently reads `1.0.5`. `v1.0.5` is already tagged, built,
and published upstream from commit `1df46bc`. The working tree on top of that
commit changes real radio behaviour (§6), so a build made now reports
`v1.0.5` on its boot banner and in `session.csv` while behaving differently
from the released `v1.0.5`.

This is precisely the ambiguity `release.yml`'s version check was written to
prevent — its own comment says shipping a binary whose boot banner disagrees
with its release tag "makes every subsequent bug report ambiguous". The check
has a blind spot: it compares the *version string* to the *tag name*, so it
catches `1.0.4` tagged as `v1.0.5`, but cannot catch `1.0.5` meaning two
different binaries at two different times.

Partly mitigated by design: `FIRMWARE_BUILD_REV` carries the git short SHA
plus `-dirty`, so a hardware report *can* be pinned to a commit if the
reporter includes it. That mitigation is doing real work here and is worth
keeping in mind as the reason this is High rather than Critical.

**Suggested resolution (not applied):** bump `version.h` to `1.0.6` before the
next build leaves this machine.

### H2 — a dispatch re-run can overwrite a published release with a different build

`release.yml`'s `workflow_dispatch` path exists to backfill assets onto an
existing tag. Its own comment is candid that it "builds whatever ref it's
checked out against (usually main), not the tagged commit itself", and names
the version check as "the only thing standing between a workflow_dispatch
mistake and the wrong binaries landing on an existing release."

That guard is currently satisfied by accident. `main`'s `version.h` says
`1.0.5` and `v1.0.5` exists, so dispatching against `v1.0.5` today would
pass the check and upload `main`'s current binaries — which are not the
binaries `v1.0.5` was cut from — onto the published release. `draft:` is
conditioned off for dispatch, so this lands directly on the public release.

This is H1's blast radius, and it is reachable from the Actions tab in two
clicks. The two findings should be considered together.

**Suggested resolution (not applied):** have the dispatch path check out the
target tag (`ref: ${{ inputs.tag }}`) rather than building the dispatch
branch, which would make the version check redundant instead of load-bearing.

---

## 3. Medium severity

### M1 — `activeChannel` is torn-readable across cores

`ChannelParams activeChannel` (`radio_task.cpp:29`) is a plain, non-`volatile`,
non-atomic struct of `{float, uint8, float, uint8, uint8}`. It is written on
Core 1 by the radio task (profile switch, `restoreHomeListen()`) and read by
value from six Core-0 call sites: `ui_actions.cpp:292`, `wifi_task.cpp:92`,
`serial_control.cpp:84`, `ui_task.cpp:311`, `ui_pages.cpp:813`, and
`radioActiveChannel()`'s other consumers.

The accessor's comment — "small POD struct, cheap to return by value" —
addresses copy *cost*, not *atomicity*. A reader can observe a mixed struct
mid-switch: new `freq_mhz` with the previous profile's `sf`/`bw_khz`/
`sync_word`.

Mostly this is cosmetic (a status page rendering a blended channel for one
frame). The case that isn't cosmetic: `ui_task.cpp:311` and
`ui_actions.cpp:292` both compute a Scope acquisition frequency from
`radioActiveChannel().freq_mhz`, so a torn read during a profile switch
would park Scope on a frequency that never existed as a coherent channel.
Bounded and self-correcting, but real.

Worth noting the exposure widened this session: the capture window means the
radio task now writes `activeChannel` (via `restoreHomeListen()`) far more
often relative to wall-clock time than it used to.

### M2 — cross-core peak masks are non-volatile with unbarriered ordering

`energyPeakBinMask[28]` (`:128`) and `energyPeakBinMaskAtComplete[28]`
(`:143`) are plain arrays. The snapshot is copied element-by-element on
Core 1 and then `energySweepCount++` (volatile) is incremented; Core 0's
logger polls that counter and, on change, reads the array through
`radioEnergyPeakBinSetAtLastComplete()`.

Correctness therefore rests on the array writes being visible before the
counter increment. Nothing enforces that — no `std::atomic`, no
`__sync_synchronize()`, no memory barrier. It works in practice on ESP32
(both cores share uncached DRAM, and the volatile store is not reordered by
the compiler *in practice*), but it is unenforced by the language.

This is flagged at Medium not because a failure has been observed, but
because **this exact code path already produced one real, hardware-only bug**
(the v0.10.1 repeat-mode Waterfall race, where every row came back quiet).
It is the project's most historically error-prone mechanism, it now carries
two more fields (`capture_bin`/`capture_count`, §6), and it is guarded only
by convention and comments.

### M3 — Pages deployment fails on every non-`main` ref *(the reported bug)*

Diagnosed from the Actions API. Signature is unambiguous:

| ref | event | steps executed | duration | result |
|---|---|---|---|---|
| `v1.0.5` (tag) | `release` | **0** | 2s | failure |
| `claude/…` branch | `workflow_dispatch` | **0** | 1s | failure |
| `main` | `workflow_run` / `workflow_dispatch` | 11 | ~6s | success |

Zero steps executed means the job was rejected before the workflow body ran
— the `github-pages` environment's deployment-branch policy only permits the
default branch. The `copilot/fix-pages-deployment-workflow` branch reaches
the same conclusion independently and quotes GitHub's literal message:
*"Tag vX.Y.Z is not allowed to deploy to github-pages."*

There is an older, separate `main` failure (run 33818794130) that died at
`Assemble site`; later `main` runs succeed, so that one appears already
resolved and is not the reported issue.

**Two candidate fixes, with a real trade-off:**

1. *Repo settings* — allow the `github-pages` environment to deploy from
   `v*` tags. One settings change, no workflow edits, keeps the protection
   model explicit. Requires UI/API access this session did not have.
2. *Workflow* — the `copilot/…` branch deletes the `environment:` block.
   That skips the check, but that block is the Pages deployment gate and
   `actions/deploy-pages` documents it as required; it also drops the
   deployment URL from the Actions UI. It trades a loud, understood failure
   for an unverified one.

A third option was applied to the working tree *before* the audit-only
instruction (§6): on a `release` event, re-dispatch the workflow against
`main` — where deploys already demonstrably pass 11/11 — and let that run do
the work. It keeps the environment gate intact and reuses the only code path
proven to work. **It is unverified**: confirming it requires publishing a
real release, which was out of scope.

### M4 — `Download dev-latest assets` is unguarded

`pages.yml` handles a missing *stable* release gracefully (`found=false`,
step skipped) but downloads `dev-latest` unconditionally. If that release is
ever absent — fresh fork, manually deleted, or deleted-and-not-yet-recreated
— `gh release download` exits non-zero and fails the whole deploy. The
asymmetry looks unintentional given the care taken on the stable path.

### M5 — four hand-rolled settings parsers, no host tests

`display_settings.cpp` (116 lines), `region_settings.cpp` (97),
`sweep_margin_settings.cpp` (97), `capture_settings.cpp` (91) each implement
their own line parse / key match / range validate / delete-then-recreate
write. None has a host test; `test/` has no `test_*_settings` directory at
all.

Their logic is *pure and trivially testable* (string in, validated struct
out) — the project already does exactly this for `gps_parse.h`, `keyboard.h`,
`ui_menu.h`. The bounds logic is where silent damage hides: an off-by-one in
a range check would accept a corrupt index and be discovered only as odd
behaviour on hardware. With four near-identical copies, a fix applied to one
and not the others is a realistic failure mode.

---

## 4. Low severity

**L1 — settings-module duplication.** The four modules above plus
`config.cpp` share an identical skeleton, down to the same bounded
`SpiBusLock lock(pdMS_TO_TICKS(2000))` and the same delete-then-recreate
rationale. The project's one-file-per-scope convention is sound and worth
keeping; the *parsing machinery* underneath it is what is duplicated. A
shared key/value helper would remove roughly 300 lines without collapsing
the scopes.

**L2 — Cell sweep never got the retune fix.** `performCellSweep()` still
calls a full `radio.begin()` per bin. Sweep's measured cost for that path was
~40.7ms/bin versus ~10.0ms/bin after the fix; Cell's 101 bins are paying the
same premium. Deliberately scoped out at the time (correctly — it kept the
change reviewable), but the two sweeps now differ structurally for no
remaining reason.

**L3 — Pass-B and the capture window compete for the same wall clock.**
Pass-B's `DISCOVERY_RX_WINDOW_MS` (2.5s) at up to `PASS_B_MAX_PEAKS_PER_SWEEP`
(8) peaks can add ~20s to a lap; measured laps of 14-20s were observed this
session at a sensitive margin. The new 2s capture window sits on top of that.
Both are individually justified and the operator explicitly accepted the
Pass-B cost, but nothing bounds their *sum*, and the two were tuned
independently.

**L4 — `radio_task.cpp` concentration.** At 2,319 lines it is the largest
module and owns Watch, Probe, Sweep, Cell, Scope, Pass-B, the bench hooks,
and now the capture window. Every race this project has found — v0.10.1's
Waterfall mask, and the two in §6 — originated here. This is an observation
about where review attention is best spent, not a request to split the file.

**L5 — untested pure helpers in `ui_pages.cpp`.** At 1,909 lines it has no
automated coverage, which is reasonable for draw code. But `menuEntryValue()`,
`sliderValueLabel()`, `sliderFraction()` and `waterfallRowToColumns()` are
pure functions over plain data. `waterfallRowToColumns()` in particular
reimplements the max-aggregation `waterfall.h`'s own
`waterfallAggregateRow()` already does and already tests — two
implementations of one rule, only one of which is covered.

**L6 — `CLAUDE.md` layout drift.** 18 real modules never appear in the
documented tree: `analyzer_budget`, `analyzer_state`, `bench_fault`,
`capture_history`, `capture_settings`, `energy_observation`, `energy_plan`,
`meshcore_identity`, `meshtastic_identity`, `node_identity`, `node_roster`,
`pass_b_plan`, `profile_state`, `region_plan`, `region_settings`,
`scope_trace`, `sweep_margin_settings`, `waterfall`. Since that file is the
first thing an agent reads, the drift compounds.

---

## 5. What is working well

Worth recording, because several of these actively caught problems this
session:

- **`FIRMWARE_BUILD_REV`.** The one thing keeping H1 from being critical.
- **Compile-time budget asserts.** `ANALYZER_STATIC_BYTES` against the
  8,192-byte ceiling, and `EnergyBinStats` against its 8B/bin budget, both
  forced a deliberate decision this session rather than silent growth
  (6,728 → 6,824 bytes, 1,368 remaining).
- **The `session_log` footprint canary.** A hardcoded expected value that
  fails when the analyzer's static size changes. It fired correctly and
  forced the growth to be reviewed against the ceiling.
- **Privacy handling.** `docs/hardware-results/private/` is gitignored with
  a stated reason (GPS precision in real captures); 209 files sit there
  untracked, verified.
- **Serial Control's design.** Operator-gated, NVS-persisted, off by
  default, bounded line grammar with CRC16, and a production/bench split
  that genuinely rejects bench opcodes in production firmware.
- **Snapshot discipline.** `radioEnergyPeakBinSetAtLastComplete()` and the
  Region "read once per sweep" rule are the right pattern, well documented,
  and M2 is a request to *enforce* what the comments already promise.
- **Comment culture.** Comments consistently explain why, cite sources and
  dates, and record what was measured. This audit was far cheaper to
  perform than it would otherwise have been.

---

## 6. Work already applied this session (for the record)

The audit instruction was "don't fix anything, just audit." These changes
predate it — they came from the `/code-review` pass that preceded the audit
request — and are listed so the working tree is not mistaken for clean. All
are **uncommitted**, build clean, and pass 207/207.

1. **`energyActive` did not cover the capture window.** That flag means "the
   energy subsystem owns the radio", and it was false for ~70% of each
   repeat cycle. Consequences: Probe/Cell/Scope mutual-exclusion checks
   passed during the window and silently queued (firing minutes later), and
   a repeat-stop press did not raise `energyCancelRequested`, so the window
   ran its full budget. Now held across the window.
2. **Capture counter could leak onto an unrelated sweep row.** Stopping
   repeat mid-window stranded that window's count; the next sweep — possibly
   a single-shot with no window at all — claimed it, producing a green
   Waterfall mark and "N PKTS" for packets it never received. Now discarded
   on repeat-loop exit.
3. **Menu `itemCount` asserts missed `ROOT_ITEMS`** — which is exactly where
   the historical v0.8.9 bug lived. Extended, and verified by deliberately
   reintroducing that bug and confirming the build fails.
4. **Sweep margin was read live per bin** rather than snapshotted per sweep,
   so a mid-lap slider change judged one lap against two thresholds. Now
   snapshotted alongside `band`.
5. **Dead constant and dead accessor** removed; the shipped default now has
   one definition instead of three.
6. **`pages.yml` re-dispatch job** for M3 — unverified, see §3.

---

## 7. Suggested order of work

1. **H1 + H2 together.** Bump `version.h`; make the dispatch path check out
   the target tag. Small, and they currently combine into a live hazard.
2. **M3** — decide between the settings fix and the workflow fix, then
   verify against a real release. Coordinate with the open
   `copilot/fix-pages-deployment-workflow` branch so the two do not conflict.
3. **M5** — host tests for the settings parsers. Cheapest real risk
   reduction available; the modules are already pure.
4. **M1/M2** — decide the intended concurrency contract and enforce it once,
   rather than per-field. These are latent, not active.
5. **L6** — refresh `CLAUDE.md`'s tree while the omissions are known.
6. Everything else as convenient.

---

## 8. Explicitly out of scope / unverified

- **No hardware verification.** The Cardputer was unplugged before the audit;
  the working-tree fixes in §6 are built and unit-tested but **not flashed**.
- **The M3 fix is unverified** and needs a real published release to confirm.
- **Not investigated:** `wifi_task.cpp`'s web UI surface and `web_assets.h`
  beyond noting their size; the RTL-SDR bench tooling under `bench/`;
  `logger_task.cpp`'s SD retry behaviour beyond confirming a retry path
  exists and that failure sets `sdReady = false` rather than buffering
  indefinitely.
- **Low confidence, not asserted as a finding:** whether PR-triggered `Build`
  runs also trigger a Pages deploy. `pages.yml`'s `workflow_run` trigger has
  no branch filter, which suggests they would, but no such deploy appears in
  the last 60 runs — every `workflow_run` deploy shows `head_branch=main`.
  Worth confirming rather than assuming either way.
- **Credential note:** the pyMC repeater API key is stored in plaintext at
  `docs/hardware-results/private/pymc_repeater_api.env` (gitignored —
  verified with `git check-ignore`). It was also pasted into a chat
  transcript. Rotating it at some convenient point would be reasonable
  hygiene; nothing suggests it has been exposed beyond that.


---

## 9. Resolution notes (v1.0.6)

**H1** — `version.h` bumped to `1.0.6` with an entry covering both the review
fixes and this audit's. `docs/STATUS.md`'s current-version line updated.

**H2** — `release.yml`'s checkout now resolves the target tag on a dispatch
(`ref: ${{ github.event_name == 'workflow_dispatch' && inputs.tag || github.ref }}`),
so a backfill builds the commit the release was cut from. The version check
survives as a second line of defence — it still catches a tag whose own
`version.h` never matched it, which the checkout cannot. Two stale comments
that described the old behaviour were corrected.

**M1** — `activeChannel`/`activeProfile` are now written and read under a
`portMUX_TYPE` spinlock, through a single `setActiveChannelLocked()` writer.
A spinlock rather than a FreeRTOS mutex because the radio task must never
block; the critical section is a ~16-byte struct copy. The two fields share
one lock so a caller sees a coherent pair.

**M2** — the completion snapshot is published with
`std::atomic_thread_fence(std::memory_order_release)` before
`energySweepCount++`, paired with an acquire fence in
`radioEnergySweepCount()`. This makes explicit the ordering the surrounding
comments already promised.

**M3** — the re-dispatch job stands. Still requires a real published release
to confirm. Coordinate with `copilot/fix-pages-deployment-workflow`, which
takes the other approach (deleting the `environment:` block).

**M4 — finding revised after reading `scripts/build_web_flasher_site.py`
more carefully.** The original framing ("asymmetric robustness — should
tolerate a missing dev-latest like it tolerates a missing stable") was
wrong: the script copies the dev track *unconditionally* and treats only
`stable` as optional, so the site genuinely cannot build without it. The
asymmetry is by design. What was actually worth fixing is the *message*:
the step now fails with an actionable `::error::` naming the cause and the
remedy (run Build on main to create `dev-latest`) instead of surfacing a
bare `gh` error.

**L6** — all 18 modules added to `CLAUDE.md`'s tree; re-checked mechanically,
no drift remains.

**M5 + L1 — done, and M5 paid for itself immediately.** The shared half of
the four parsers moved to a pure `config_line.h`; each module's `apply...()`
moved into its header, which is what made them host-testable
(`test_config_line/`, `test_settings_parse/`, 20 new cases, 207 → 227; the
`.cpp` files went 401 → 293 lines).

Writing the first test found a live defect that four copies and zero
coverage had hidden: Arduino's `String::toInt()` yields 0 for unparseable
input, and 0 is a *valid* value for `window_index` (Capture: OFF) and
`idle_timeout_index` (Idle dim: Off). A corrupt line in either file was
therefore honoured as a real setting rather than ignored — capture could be
silently disabled by a damaged card. `configParseLong()` now requires the
whole token to be digits. Verified on hardware afterwards that the four real
files on the card still load their persisted values, three of which are
non-defaults (`margin=300`, `brightness=40`, `idle_idx=1`) — so the parser
is reading them, not falling back.

**Still open.** `L2`
(Cell's retune) is a genuine radio-behaviour change that should not ship
unverified while the hardware is disconnected; the Sweep-side proof makes it
low-risk but not zero-risk. `L3`/`L4`/`L5` are structural observations
rather than defects.


---

## 10. L2 attempted and reverted — the light retune costs measurement accuracy

Porting `performEnergySweep()`'s light retune (`standby()` + `setFrequency()`
+ `startReceive()`) to `performCellSweep()` was implemented, measured on real
hardware, and **reverted**. Recording it here because the negative result is
more useful than the change would have been.

**Method.** Six back-to-back Cell laps per configuration, minutes apart, same
antenna and location, comparing both duration and the strongest signal found
(`radioCellStrongestSignal()`).

| Config | Duration (avg) | Strongest signal found |
|---|---|---|
| Full `begin()` per bin (shipped) | 5,594ms (55.4ms/bin) | **892.000MHz at -72..-74dBm, 6/6 laps** |
| Light retune | 1,423ms (14.1ms/bin) — **3.9x faster** | 870.0 / 877.5 / 885.0 / 877.5 / 882.0 / 886.0MHz, only -86..-91dBm — **never found 892MHz** |
| Light retune + 5ms settle | 2,150ms — 2.6x faster | 884.000MHz at -72dBm, 5/6 laps |

**The speedup is real and so is the damage.** The light retune ran 3.9x
faster and never once saw the band's strongest real carrier — a stable
-73dBm emitter the baseline found on every single lap — reporting
noise-floor peaks at a different frequency each time instead. A 15-18dB
under-read of a real carrier disqualifies it: Cell exists to answer "is
there cell-band energy here", logs every bin's absolute RSSI, and has no
relative threshold to hide a systematic under-read behind the way Sweep's
rolling noise floor does.

**Root cause looks like settling time.** Adding a 5ms delay after
`startReceive()` restored stable, strong readings (-72dBm) while still
running 2.6x faster than baseline — strong evidence that `begin()`'s
incidental overhead was giving the AGC/RSSI time to settle, and the light
path samples too early. That is a promising route for a future attempt.

**But it is not proven equivalent, which is why nothing shipped.** The
settled variant consistently reports 884.000MHz as strongest, while the
baseline consistently reports 892.000MHz — two real emitters within ~1dB of
each other, with the two configurations disagreeing about which wins. Close
is not the same as calibrated.

**Acceptance test for any future attempt:** compare full per-bin RSSI curves
between configurations, not just the strongest bin. Requires reading
`cell.csv` off the card or adding a bench-only per-bin readback (the
`BENCH_SWEEP_FLOOR` mechanism already does exactly this for Sweep and could
be mirrored). "Same strongest bin" is too weak a signal to accept on.

### M6 (new) — does Sweep's shipped light retune have the same problem?

Raised by the above, not yet investigated. `performEnergySweep()` has used
the light retune since `v1.0.2` with **no settling delay**, so it plausibly
samples RSSI just as early as the Cell port did.

Why it may not have been noticed: Pass A's peak test is *relative* —
`energyBinIsPeak()` compares a bin's peak against a rolling noise floor
built from the same possibly-depressed samples — so a roughly uniform
under-read largely cancels out, and peaks kept firing. Capture, Waterfall
and Pass-B all continued to work through this session's testing.

What could still be wrong:
- `energy.csv`'s absolute `rssi_avg_dbm`/`rssi_peak_dbm` may be understated
  since v1.0.2, which matters for any post-hoc analysis of logged runs.
- Sensitivity to genuinely strong, short signals could be reduced.
- The 923MHz-edge rolloff characterisation in §"What's hardware-verified"
  was performed *before* v1.0.2, with a full `begin()` per bin, so its
  conclusions do not automatically carry over to the shipped retune path.

**Suggested check:** run one Sweep with the light retune and one with a
forced full `begin()` per bin against the same injected carrier
(`scripts/phase9_edge_carrier_bench.py` already parks and measures), and
compare reported RSSI. If they differ materially, a settling delay in
`performEnergySweep()` is the same one-line fix that worked for Cell here.


---

## 11. M6 Phase 1 result — the offset is real, but the dangerous case is still open

`BENCH_SWEEP_RETUNE` (bench image only, `FULL`|`LIGHT`) was added so both
retune strategies run on **one firmware image in one session**, alternating
FULL/LIGHT within each repeat. After the number of confounded measurements
this session produced, comparing two separate builds across two flashes was
not good enough. `scripts/phase9_retune_floor_bench.py` drives it.

Three alternating pairs, no transmitter, no signal — just the reported
noise floor across all 85 US-band bins (`BENCH_SWEEP_FLOOR`):

| Mode | Mean floor | Spread across runs | Sweep duration |
|---|---|---|---|
| `FULL` (pre-v1.0.2 begin-per-bin) | **-119.93 dBm** | 0.54 dB | 5,360ms |
| `LIGHT` (shipped since v1.0.2) | **-122.77 dBm** | 0.05 dB | 833ms |

**`LIGHT - FULL = -2.84 dB`**, far outside either arm's run-to-run spread
(and `LIGHT` is remarkably repeatable at 0.05 dB). So the light retune
**does** change what Sweep reports. M6 is confirmed, not hypothetical.

**What this establishes:** `energy.csv`'s absolute `rssi_avg_dbm` /
`rssi_peak_dbm` values have been ~2.8 dB low at the noise floor since
v1.0.2. Any analysis comparing logged runs across that boundary is
comparing two different measurement regimes.

**What it deliberately does not establish — and this is the part that
matters.** -2.84 dB at the *noise floor* is small next to the 15-18 dB by
which the same retune under-read a real carrier on Cell. That difference is
itself the evidence for the signal-dependent hypothesis: settling at the
noise floor needs almost no AGC gain change, so the error is small; a strong
carrier demands a large gain change, so the error is large. If that holds
for Sweep, the consequence is worse than a logging inaccuracy — a signal
that genuinely sits 40 dB over the floor could report as 25 dB over it and
fall *below* the 35 dB margin, i.e. go undetected. Pass A's relative
comparison does not protect against an error that scales with signal
strength.

Suggestive but far too small a sample to lean on: across these six sweeps
`FULL` logged the only peak (1, in its first run); `LIGHT` logged none.

**Phase 2 (needs the Heltec rig, not currently connected).** Inject a known
carrier — `LONG_MODERATE` at 912.8125MHz, the fixture Phase 8/9 already use
— and compare, between `FULL` and `LIGHT` on the same image:
1. reported peak RSSI at the target bin, and
2. detection rate (`WP`) over many laps under identical stimulus.

`scripts/phase9_sweep_margin_bench.py` already arms the transmitter through
a sweep and counts `WP`; it needs only the retune-mode switch rather than
the margin sweep. The RTL-SDR gives independent ground truth for the
carrier's real level, so the question becomes "which mode reports it
correctly", not merely "which differs".

**Also worth testing in Phase 2:** whether a short settling delay after
`startReceive()` closes the gap, as it did for Cell (5 ms restored stable
-72 dBm readings there). If it recovers both the floor offset *and*
strong-signal accuracy, it is a one-line fix that keeps most of the ~4x
speedup. Confirming it against the floor alone would not be sufficient.

**Not changed pending Phase 2.** The light retune stays shipped: its speed
benefit is real and measured, the confirmed harm so far is a 2.84 dB floor
offset, and reverting on partial evidence would discard a verified
improvement. The margin recalibration question (the 35 dB default was
calibrated pre-v1.0.2, under `FULL`) should be revisited once Phase 2 says
how large the strong-signal error actually is.


---

## 12. M6 Phase 2, attempt 1 — inconclusive: injected carrier arrived ~67dB too weak

Tooling is built and working (`BENCH_SWEEP_RETUNE`,
`scripts/phase9_retune_carrier_bench.py`); the run itself could not answer
the question, and the reason is worth recording so the next attempt does not
repeat it.

**The transmitter was fine.** The Heltec accepted `CONFIG LONG_MODERATE` and
emitted 143 beacon pulses in run 1 (plus 25 in a follow-up), every one
logged `#BEACON <n> LONG_MODERATE 912813 OK`. Transmit side verified, not
assumed.

**The receive level was the problem.** Best carrier reading across seven
sweeps was **-101.7dBm**, with most runs at -115 to -118dBm against a
-120/-122dBm floor. The documented bench for this same fixture (docs/STATUS.md's
923MHz-edge injected-carrier work) recorded **-34.7dBm** with "matched
915MHz whip antennas, physically separated desktop/under-desk". This run
therefore received the carrier roughly **67dB weaker** than the setup this
test was designed around — most likely an antenna or placement difference,
since the transmitter is confirmed firing at its usual -9dBm cap.

That level is disqualifying for this particular question. M6 asks whether
the light retune suppresses *strong* signals more than it suppresses the
floor; a carrier sitting 2-18dB over the floor is not a strong signal, and
cannot exercise the large AGC gain change the hypothesis is about.

Raw numbers, recorded but **not** treated as evidence:

| Mode | carrier over floor, per run | best |
|---|---|---|
| `FULL` | +2.0, +2.0, +18.6, +4.5 dB | +18.6 dB (-101.7dBm) |
| `LIGHT` | +5.5, +0.3, +5.9 dB | +5.9 dB (-116.8dBm) |

The direction happens to match the hypothesis, and it would be easy to
present that as a result. It is not one: n is tiny, the single strong `FULL`
catch is one sweep, and whether any given bin visit overlaps a 2s-interval
pulse is substantially luck. Reporting this as confirmation would be the same
mistake that made the earlier dwell-timing A/B look "inconclusive" when it
was really just under-powered.

**Independent ground truth was unavailable.** The RTL-SDR had been
disconnected by the time this ran (USB enumerated only the two Espressif
devices), so the on-air level could not be cross-checked against the
Cardputer's own reading — which is exactly the check that would separate
"weak transmit/coupling" from "Cardputer receive path".

**For the next attempt:**
1. Confirm the Heltec has its antenna fitted and is positioned like the
   documented bench (both on matched 915MHz whips, separated but in the
   same room). Target a received level near -35dBm, not -110dBm.
2. Reconnect the RTL-SDR and measure the on-air level first, so the question
   is "which mode reports it correctly" rather than "which mode differs".
3. Re-run `phase9_retune_carrier_bench.py` (now hardened against the
   transient USB-CDC frame drop that ended run 1 seven sweeps in).
4. Consider raising `--repeats`; with a 2s pulse interval and ~35% overlap
   probability per lap, 4 pairs is too few to separate modes even at a
   healthy signal level.


---

## 13. M6 — floor offset confirmed; the signal-dependent claim RETRACTED

**This section replaces an earlier version of itself that declared the
signal-dependent under-read confirmed. That conclusion did not survive its
own control and is withdrawn.**

### What is solid

The noise-floor offset is real and among the most reproducible numbers in
this project. Across **five independent runs** on different rigs and
antenna setups today, every single one reported:

| | `FULL` | `LIGHT` | difference |
|---|---|---|---|
| 85-bin median noise floor | **-120.30 dBm** | **-122.70 dBm** | **-2.40 dB** |

Identical to two decimal places, every time. `energy.csv`'s absolute RSSI
has been ~2.4dB low since v1.0.2. That part of M6 stands.

### What is not solid, and why

The claim that the under-read *grows with signal strength* — the part that
would mean lost detections rather than just wrong log values — is not
supported. Three attempts produced three different answers:

| Attempt | `FULL` carrier | `LIGHT` carrier | Apparent conclusion |
|---|---|---|---|
| Batch 1 (Pass-B contaminated) | best -101.7 dBm | best -116.8 dBm | LIGHT much worse |
| Batch 2 (Pass-B contaminated) | best -94.2 dBm | best **-73.5 dBm** | LIGHT much *better* |
| Pass-B suppressed | -104.42 dBm (tight) | -115.57 dBm (tight) | LIGHT ~11dB worse |
| **settle=0 control** (same config as the row above) | **-115.5 dBm (tight)** | mostly floor, one -99.0 dBm | **did not reproduce** |

The control is the decisive row. It re-ran the *same* configuration that
produced the "confirmed" result and did not reproduce it: `FULL`'s own
carrier reading moved from a tightly-clustered -104.3 dBm to a
tightly-clustered -115.5 dBm between runs. **An 11dB shift within one arm,
between runs — the same magnitude as the effect being claimed.** Whatever is
moving (antenna coupling, transmitter state, phase between the 2s pulse
train and the sweep cadence) swamps the comparison.

### The methodological error

Reading a **max statistic over a luck-dominated distribution**. A 2s pulse
train sampled by a 10-55ms bin visit, once per sweep, means most sweeps see
nothing and a few see a pulse. "Best of 6" then measures which arm got
luckiest, not which measures correctly — and it flips between runs. Tight
clustering *within* a run made each result look convincing in isolation,
which is exactly how it was called three times in three directions.

### What a trustworthy measurement needs

1. **Raise the duty cycle** so nearly every bin visit sees the carrier.
   `BEACON` is fixed at 2s; driving `ARM` at ~0.5s intervals (as
   `phase9_sweep_margin_bench.py` already does) with ~400ms airtime gives
   ~80% occupancy instead of ~20%.
2. **Report the distribution, not the max** — median and quartiles over
   20+ sweeps per arm, with the catch/no-catch split stated explicitly.
3. **Bracket each run with a FULL reference** so between-run drift is
   measurable rather than assumed away, which is what the settle=0 control
   was for and why it earned its keep.

### Status of the fix

`BENCH_SWEEP_SETTLE` (0-50ms) is built and works, but
`ENERGY_SWEEP_SETTLE_DEFAULT_MS` ships at **0 — disabled**. A 5ms settle
costs ~425ms per 85-bin sweep; paying that for an unproven benefit is not
justified. The knob remains so the question can be answered properly.
