#!/usr/bin/env python3
"""M6 Phase 1 — does Sweep's light per-bin retune shift the reported RSSI?

Ports of that retune to Cell made it miss a real -73dBm carrier while still
reporting plausible noise, which looks like RSSI/AGC settling time that
radio.begin()'s overhead had been providing (docs/research/
2026-09-04-project-audit.md, L2/M6). This asks the cheap half of the
question for Sweep: with no transmitter and no signal, does the reported
per-bin noise floor differ between the two retune strategies?

A uniform offset here would be mostly harmless to Pass A -- energyBinIsPeak()
compares each bin against a rolling floor built from the same samples, so a
constant shift cancels. It would still mean energy.csv's absolute values are
wrong. What this CANNOT see is the dangerous case: a signal-dependent
under-read that suppresses strong carriers specifically. That needs an
injected carrier (Phase 2, the Heltec rig) -- see the audit doc.

Alternates FULL/LIGHT within one session on one firmware image via
BENCH_SWEEP_RETUNE, so the two arms cannot differ by build, flash or boot.
Requires the cardputer-adv-bench image with Serial Control enabled.

Usage:
  phase9_retune_floor_bench.py --port /dev/ttyACM0 --repeats 3 \
      --log <path> [--results <path.jsonl>]
"""
import argparse
import json
import pathlib
import statistics
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from bench_harness import CARD_MARKER, Endpoint, ResultWriter, card_status, require_ack

SWEEP_TERMINAL_TIMEOUT_S = 180.0
STATUS_POLL_INTERVAL_S = 0.3


def run_sweep(card):
    before = card_status(card)
    if before.get("W") == "RUNNING":
        raise RuntimeError(f"Sweep already active: {before}")
    require_ack(card, "SWEEP_START", "-")
    deadline = time.monotonic() + SWEEP_TERMINAL_TIMEOUT_S
    status = None
    while time.monotonic() < deadline:
        status = card_status(card)
        if status.get("W") in {"COMPLETE", "CANCELLED", "FAILED"}:
            break
        time.sleep(STATUS_POLL_INTERVAL_S)
    else:
        raise TimeoutError("sweep never reached a terminal state")
    if status.get("W") != "COMPLETE":
        raise RuntimeError(f"sweep ended {status.get('W')}: {status}")
    return status


def read_floor_curve(card, bin_count):
    """Every bin's average RSSI (tenths of dBm) from the sweep just finished."""
    curve = {}
    for b in range(bin_count):
        opcode, payload = card.request("BENCH_SWEEP_FLOOR", str(b), timeout=6.0)
        if opcode != "ACK":
            raise RuntimeError(f"BENCH_SWEEP_FLOOR bin {b} failed: {opcode} {payload}")
        fields = dict(item.split("=", 1) for item in payload.split(";"))
        curve[b] = int(fields["FLOOR"])
    return curve


def summarise(curve):
    vals = [v / 10.0 for v in curve.values()]
    return {
        "bins": len(vals),
        "mean_dbm": statistics.fmean(vals),
        "median_dbm": statistics.median(vals),
        "min_dbm": min(vals),
        "max_dbm": max(vals),
        "stdev_db": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--repeats", type=int, default=3,
                    help="FULL/LIGHT pairs; alternated so drift hits both arms equally")
    ap.add_argument("--log", required=True)
    ap.add_argument("--results")
    args = ap.parse_args()

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None

    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.port, CARD_MARKER, log)
        try:
            identity = require_ack(card, "HELLO", "-", timeout=20.0)
            print("identity:", identity)
            if "BENCH=1" not in identity:
                raise RuntimeError(
                    f"not the cardputer-adv-bench image (BENCH_SWEEP_RETUNE/FLOOR need it): {identity}")

            per_mode = {"FULL": [], "LIGHT": []}
            for rep in range(args.repeats):
                # Alternate within each repeat so slow ambient drift is shared
                # between arms rather than loading onto whichever ran last.
                for mode in ("FULL", "LIGHT"):
                    payload = require_ack(card, "BENCH_SWEEP_RETUNE", mode)
                    if f"RETUNE={mode}" not in payload:
                        raise RuntimeError(f"retune mode not applied: {payload}")
                    status = run_sweep(card)
                    bin_count = int(status.get("WN", "0"))
                    curve = read_floor_curve(card, bin_count)
                    summary = summarise(curve)
                    summary.update({"mode": mode, "repeat": rep,
                                    "away_ms": int(status.get("EA", "0")),
                                    "peaks": int(status.get("WP", "0"))})
                    per_mode[mode].append(summary)
                    print(f"  rep{rep} {mode:<5} away={summary['away_ms']:>5}ms "
                          f"mean={summary['mean_dbm']:7.2f}dBm "
                          f"median={summary['median_dbm']:7.2f} "
                          f"min={summary['min_dbm']:7.1f} max={summary['max_dbm']:7.1f} "
                          f"peaks={summary['peaks']}")
                    if results:
                        results.write({"event": "sweep", **summary,
                                       "curve_dbm_x10": curve})

            print()
            out = {"event": "summary"}
            for mode, runs in per_mode.items():
                means = [r["mean_dbm"] for r in runs]
                aways = [r["away_ms"] for r in runs]
                out[mode] = {"mean_of_means_dbm": statistics.fmean(means),
                             "spread_db": max(means) - min(means),
                             "mean_away_ms": statistics.fmean(aways)}
                print(f"{mode:<5} mean floor {out[mode]['mean_of_means_dbm']:7.2f}dBm "
                      f"(spread {out[mode]['spread_db']:.2f}dB across {len(runs)} runs), "
                      f"avg {out[mode]['mean_away_ms']:.0f}ms/sweep")
            delta = out["LIGHT"]["mean_of_means_dbm"] - out["FULL"]["mean_of_means_dbm"]
            out["light_minus_full_db"] = delta
            print(f"\nLIGHT - FULL = {delta:+.2f} dB on the reported noise floor")
            worst_spread = max(out["FULL"]["spread_db"], out["LIGHT"]["spread_db"])
            if abs(delta) <= worst_spread:
                print("=> within run-to-run spread: no uniform floor offset detected.")
            else:
                print("=> exceeds run-to-run spread: a real floor offset between modes.")
            print("Either way this says nothing about strong signals -- run Phase 2 "
                  "with the Heltec for that.")
            if results:
                results.write(out)
        finally:
            card.port.close()
            if results:
                results.close()


if __name__ == "__main__":
    main()
