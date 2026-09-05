#!/usr/bin/env python3
"""Offline, deterministic report over phase12_focus_matrix.py trial rows.

Reads the JSONL the matrix runner wrote and answers one question per arm: does
a fixed RSSI condition separate source-on from source-off trials, and with what
confidence? It reports proportions as Wilson 95% intervals rather than bare
percentages (docs/research/phase12-survey-truth-design.md §6.2 item 5, NIST
1.3.5 guidance) because a "30/30" at these counts is not the certainty it looks
like.

It touches no device and mutates no input. Given the same file it prints the
same report, so a result can be re-derived rather than remembered.

What it deliberately does NOT do: name a coverage label, call an arm's
source-off trials "quiet", or pick thresholds the operator has not accepted. A
separating threshold printed here is a *candidate* for §3.1, and it is only
meaningful for the frequency offset its arm actually ran at.
"""

import argparse
import collections
import json
import math
import pathlib
import statistics
import sys


def wilson_interval(successes, total, z=1.959963984540054):
    """Two-sided Wilson score interval; correct at 0/n and n/n where normal isn't."""
    if total == 0:
        return (0.0, 1.0)
    p = successes / total
    denom = 1.0 + z * z / total
    center = (p + z * z / (2 * total)) / denom
    margin = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denom
    return (max(0.0, center - margin), min(1.0, center + margin))


def load_trials(path):
    trials, meta = [], None
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                raise SystemExit(f"{path}:{line_number}: {error}")
            if row.get("event") == "boot" and meta is None:
                meta = row
            elif row.get("event") == "trial":
                trials.append(row)
    return trials, meta


def best_threshold(on_values, off_values):
    """Lowest 1 dB threshold that admits every source-on trial and no source-off one.

    Returns (threshold_dbm_x10, margin_db) or None when the two sets overlap —
    and an overlap is a real answer: it says this arm cannot support a fixed
    RSSI condition, which is exactly what §3.1 needs to know before choosing
    one.
    """
    if not on_values or not off_values:
        return None
    if min(on_values) <= max(off_values):
        return None
    # Any cut in the gap separates; take the midpoint, rounded to whole dB.
    low, high = max(off_values), min(on_values)
    threshold = int(round((low + high) / 2.0 / 10.0)) * 10
    threshold = max(low + 1, min(high, threshold))
    return threshold, (high - low) / 10.0


def summarize(values):
    if not values:
        return "n/a"
    return (f"min {min(values) / 10:7.1f}  med {statistics.median(values) / 10:7.1f}  "
            f"max {max(values) / 10:7.1f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=pathlib.Path, help="JSONL from phase12_focus_matrix.py")
    parser.add_argument("--metric", choices=("p90", "peak", "median"), default="p90",
                        help="RSSI summary the candidate condition is built on")
    args = parser.parse_args()

    if not args.results.exists():
        raise SystemExit(f"no such results file: {args.results}")
    trials, meta = load_trials(args.results)
    if not trials:
        raise SystemExit(f"{args.results} contains no trial rows")

    field = {"p90": "p90_dbm_x10", "peak": "peak_dbm_x10", "median": "median_dbm_x10"}[args.metric]

    if meta:
        print(f"build     : {meta.get('identity', 'unknown')}")
        print(f"plan      : positions={meta.get('positions')} dwells_ms={meta.get('dwells_ms')} "
              f"trials/state={meta.get('trials_per_state')} order={meta.get('order')}")
    print(f"metric    : {args.metric} (dBm)")
    print(f"trial rows: {len(trials)}\n")

    arms = collections.defaultdict(lambda: {"on": [], "off": [], "away": [], "rows": []})
    for row in trials:
        arm = arms[(row["position"], row["dwell_ms"])]
        arm["on" if row["source_on"] else "off"].append(row[field])
        arm["away"].append(row["radio_away_ms"])
        arm["rows"].append(row)

    unresolved = []
    for (position, dwell), arm in sorted(arms.items(), key=lambda item: (item[0][0], item[0][1])):
        sample = arm["rows"][0]
        on, off = arm["on"], arm["off"]
        print(f"=== {position} @ {dwell} ms  "
              f"(tx {sample['tx_mhz']:.4f} MHz -> bin {sample['bin']} "
              f"center {sample['bin_center_mhz']:.3f}, offset {sample['offset_khz']:+.1f} kHz)")
        print(f"  source-on  n={len(on):3d}  {summarize(on)}")
        print(f"  source-off n={len(off):3d}  {summarize(off)}")
        print(f"  radio-away min {min(arm['away'])} ms, max {max(arm['away'])} ms")

        selection = best_threshold(on, off)
        if selection is None:
            overlap = "no trials" if not on or not off else (
                f"source-on min {min(on) / 10:.1f} <= source-off max {max(off) / 10:.1f}")
            print(f"  candidate condition: NONE — the two sets overlap ({overlap}).")
            print("  This arm does not support a fixed RSSI condition; it is not evidence "
                  "that the frequency was quiet.")
            unresolved.append((position, dwell))
        else:
            threshold, margin = selection
            hits_on = sum(1 for value in on if value >= threshold)
            hits_off = sum(1 for value in off if value >= threshold)
            lo_on, hi_on = wilson_interval(hits_on, len(on))
            lo_off, hi_off = wilson_interval(hits_off, len(off))
            print(f"  candidate condition: {args.metric} >= {threshold / 10:.1f} dBm "
                  f"(separating margin {margin:.1f} dB)")
            print(f"    source-on  detected {hits_on}/{len(on)}  "
                  f"95% CI [{lo_on:.3f}, {hi_on:.3f}]")
            print(f"    source-off detected {hits_off}/{len(off)}  "
                  f"95% CI [{lo_off:.3f}, {hi_off:.3f}]")
        print()

    print("Interpretation limits:")
    print("  - A separating threshold is a candidate for §3.1, not an accepted constant,")
    print("    and it is valid only at the frequency offset its arm ran at.")
    print("  - Source-off trials are ambient observation on this bench, not a calibrated")
    print("    false-hit rate (§6.1): the control path is not known quiet.")
    print("  - No arm here licenses a coverage label or a 'no activity' conclusion.")
    if unresolved:
        print(f"  - {len(unresolved)} arm(s) did not separate: "
              + ", ".join(f"{p}@{d}ms" for p, d in unresolved))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f"phase12 Focus matrix report: {error}", file=sys.stderr)
        sys.exit(2)
