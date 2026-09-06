#!/usr/bin/env python3
"""Phase 12 step 4: what should the `sampled` / `repeated` coverage thresholds be?

This is the one W12 item that still needs new measurement. Every other open
question turned out to be a product decision (the away-time budget, and whether
Focus reports activity at all); coverage is different because it is defined
over *repeated* requests, and no single-pass matrix can supply it
(docs/research/phase12-survey-truth-design.md §8, step 4).

What it does
------------
Drives N repeated Focus requests at one bin, at each of several dwells, without
transmitting anything. For every request it records the terminal status, home
restore, observation time actually achieved, sample count, and the qualifying
count -- then reports, per dwell, how many passes it takes to accumulate a
given observation time and how stable the qualifying fraction is across them.

That is deliberately all it does. It measures what repeated passes *yield*; it
does not pick the thresholds, because picking them is a judgement about what an
operator should be told a bin has been "covered" enough to claim. The output is
the input to that judgement.

Two properties this campaign is specifically looking for:

  1. **Does a pass's yield hold across repeats?** If pass 7 of 10 returns
     materially fewer accepted samples than pass 1, accumulated observation
     time is not a sound basis for a coverage label and the threshold has to be
     expressed in valid passes instead.
  2. **Does a longer dwell beat more short passes?** Same total observation
     time reached two ways. §6.4 already found a 100 ms pass cannot both catch
     a source and reject ambient no matter the threshold, so if short passes do
     not accumulate into something better, `sampled` has a dwell floor and not
     only a time floor.

Non-transmitting by design. This runs against ambient, so it characterises the
instrument's repeatability rather than its detection rate -- detection was
already measured (§6.4) and is not what a coverage label reports. There is no
--allow-transmit path here on purpose.

Usage
-----
    python3 scripts/phase12_coverage_campaign.py \
        --cardputer-port /dev/ttyACM0 \
        --log run.log --results coverage.jsonl \
        --bin 43 --repeats 20 --dwells 250,500,1000,2000

Requires the bench image (BENCH=1); production rejects BENCH_FOCUS.
"""

import argparse
import pathlib
import statistics
import sys
import time

import serial  # noqa: F401  (imported for the same failure mode as its siblings)

from bench_harness import (CARD_MARKER, Endpoint, ResultWriter, card_status,
                           parse_fields, require_ack, wait_for)

# focus_plan.h: samples derive from dwell at a fixed 20 ms spacing rather than
# being requested independently. The fixture mirrors that instead of choosing
# its own, so a pass here is the same shape as a pass in the field.
FOCUS_SAMPLE_SPACING_MS = 20


def samples_for_dwell(dwell_ms):
    derived = (dwell_ms + FOCUS_SAMPLE_SPACING_MS - 1) // FOCUS_SAMPLE_SPACING_MS + 1
    return max(derived, 2)


def one_pass(card, bin_index, dwell_ms):
    """Request one Focus pass; return its result fields once durably written."""
    before = card_status(card)
    before_written = int(before.get("FW", "0"))
    before_enqueued = int(before.get("FO", "0"))
    samples = samples_for_dwell(dwell_ms)

    require_ack(card, "BENCH_FOCUS", f"{bin_index}:{dwell_ms}:{samples}")
    terminal = wait_for(
        card,
        lambda s: s.get("FS") == "3"
        and int(s.get("FO", "0")) > before_enqueued
        and int(s.get("FW", "0")) > before_written,
        max(12.0, dwell_ms / 1000.0 + 10.0),
        "Focus completion, restore, and focus.csv commit",
    )
    result = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "-"))
    # The qualifying count is not in the result frame: BENCH_FOCUS_COUNTS
    # returns a ladder of counts above the pass's own median, so a host can
    # evaluate any margin offline instead of reflashing per candidate. C6 is
    # the arm that survived §6.4's campaigns.
    counts = parse_fields(require_ack(card, "BENCH_FOCUS_COUNTS", "-"))
    return terminal, result, counts, samples


def summarise(rows, dwell_ms):
    """Per-dwell view of what repeated passes actually yielded."""
    complete = [r for r in rows if r["status"] == "0" and r["home_restore"] == "1"]
    observed = [r["observation_ms"] for r in complete]
    accepted = [r["sample_count"] for r in complete]
    qualifying = [r["qualifying_count"] for r in complete]

    # Qualifying count as a fraction of accepted samples: §6.4 found the rule
    # transfers between dwells in that form, not as an absolute count.
    fractions = [q / a for q, a in zip(qualifying, accepted) if a > 0]

    out = {
        "dwell_ms": dwell_ms,
        "requested_passes": len(rows),
        "valid_passes": len(complete),
        "restore_failures": sum(1 for r in rows if r["home_restore"] != "1"),
        "non_complete": sum(1 for r in rows if r["status"] != "0"),
        "observation_ms_total": sum(observed),
        "observation_ms_median": statistics.median(observed) if observed else 0,
        "sample_count_median": statistics.median(accepted) if accepted else 0,
    }
    if len(observed) >= 2:
        out["observation_ms_stdev"] = statistics.stdev(observed)
    if fractions:
        out["qualifying_fraction_median"] = statistics.median(fractions)
        out["qualifying_fraction_min"] = min(fractions)
        out["qualifying_fraction_max"] = max(fractions)
    if len(fractions) >= 2:
        out["qualifying_fraction_stdev"] = statistics.stdev(fractions)
    # Passes needed to accumulate each candidate time floor. This is the shape
    # a `sampled` threshold would take, reported rather than chosen.
    for floor_ms in (2000, 5000, 10000):
        need, acc = None, 0
        for i, ms in enumerate(observed, start=1):
            acc += ms
            if acc >= floor_ms:
                need = i
                break
        out[f"passes_to_{floor_ms}ms"] = need
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw framed-control log")
    parser.add_argument("--results", help="append-only JSONL summary path")
    parser.add_argument("--bin", type=int, default=43,
                        help="US Sweep bin index to survey (default 43 = 912.750 MHz)")
    parser.add_argument("--repeats", type=int, default=20,
                        help="passes per dwell (default 20)")
    parser.add_argument("--dwells", default="250,500,1000,2000",
                        help="comma-separated dwell values in ms")
    parser.add_argument("--profile", default="MESHTASTIC",
                        help="profile to resolve home from; the bench image pins home "
                             "into the Meshtastic block only (empty to leave as-is)")
    parser.add_argument("--settle-s", type=float, default=1.0,
                        help="pause between passes, so Watch is genuinely restored between them")
    args = parser.parse_args()

    dwells = [int(d) for d in args.dwells.split(",") if d.strip()]
    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    writer = ResultWriter(args.results) if args.results else None
    # Endpoint takes a raw file handle, not a ResultWriter: it calls .flush()
    # on every framed line. Same pattern as the other phase12 fixtures.
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        try:
            return run_campaign(card, args, dwells, writer)
        finally:
            card.close()
            if writer is not None:
                writer.close()


def run_campaign(card, args, dwells, writer):
        # A generous first handshake: the bench image can still be finishing
        # boot when the port enumerates. BENCH is a HELLO field, not a STATUS
        # one — checking the wrong frame silently reads as "not bench".
        hello = parse_fields(require_ack(card, "HELLO", "-", timeout=45.0))
        if hello.get("BENCH") != "1":
            raise SystemExit("bench image required: production rejects BENCH_FOCUS "
                             f"(HELLO said {hello})")
        status = card_status(card)
        if status.get("SD") != "1":
            raise SystemExit("SD not ready: focus.csv commit is this fixture's completion signal")

        # Focus receives at the *home* bandwidth, so the resolved home channel
        # is part of the measurement, not scenery. The bench image pins home
        # into the build, but only for the Meshtastic block — a device left on
        # MeshCore silently surveys at a different bandwidth (design entry
        # §"bench home" note). Switch, and record what we actually ran at.
        if args.profile:
            require_ack(card, "PROFILE_SET", args.profile)
            status = card_status(card)
        home_khz = int(status.get("F", "0"))
        print(f"bench image {hello.get('R', '?')}, profile {status.get('P', '?')}, "
              f"home {home_khz / 1000.0:.3f} MHz")
        if writer is not None:
            writer.write({"event": "context", "hello": hello, "status": status,
                          "bin": args.bin, "repeats": args.repeats, "dwells": dwells})

        summaries = []
        for dwell_ms in dwells:
            rows = []
            for n in range(args.repeats):
                terminal, result, counts, samples = one_pass(card, args.bin, dwell_ms)
                row = {
                    "pass": n + 1,
                    "dwell_ms": dwell_ms,
                    "requested_samples": samples,
                    "status": result.get("RS", ""),
                    "home_restore": result.get("HR", ""),
                    "observation_ms": int(result.get("OBS", "0")),
                    "sample_count": int(result.get("N", "0")),
                    "median_dbm_x10": int(result.get("MED", "0")),
                    # Whole ladder kept, not just C6: the campaign should not
                    # bake in a margin the analysis may want to revisit.
                    "counts_above_median": {k: int(v) for k, v in counts.items()
                                            if k.startswith("C")},
                    "qualifying_count": int(counts.get("C6", "0")),
                    "away_ms": int(terminal.get("FA", "0")),
                }
                rows.append(row)
                if writer is not None:
                    writer.write({"event": "pass", **row})
                print(f"  dwell {dwell_ms:>5}ms  pass {n + 1:>3}/{args.repeats}  "
                      f"obs {row['observation_ms']:>5}ms  n={row['sample_count']:>3}  "
                      f"qc={row['qualifying_count']:>3}  rs={row['status']}")
                time.sleep(args.settle_s)

            summary = summarise(rows, dwell_ms)
            summaries.append(summary)
            if writer is not None:
                writer.write({"event": "dwell_summary", **summary})
            print(f"dwell {dwell_ms}ms: {summary['valid_passes']}/{summary['requested_passes']} valid, "
                  f"median obs {summary['observation_ms_median']}ms, "
                  f"qualifying fraction median "
                  f"{summary.get('qualifying_fraction_median', float('nan')):.3f}")

        print("\n--- what this does and does not settle ---")
        print("Reported: yield and repeatability of repeated passes per dwell.")
        print("NOT reported: a coverage threshold. Choosing one is a judgement")
        print("about what an operator should be told is 'covered' -- these")
        print("numbers are its input, not its answer (design entry §8 step 4).")
        return 0


if __name__ == "__main__":
    sys.exit(main())
