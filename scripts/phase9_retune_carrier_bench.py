#!/usr/bin/env python3
"""M6 Phase 2 — does Sweep's light retune under-report a REAL carrier?

Phase 1 (phase9_retune_floor_bench.py) showed the light retune reports the
noise floor 2.84dB low. That is small and, being uniform, mostly cancels in
Pass A's relative peak test. The dangerous case it could not see is a
signal-DEPENDENT under-read: on Cell the same retune missed a real -73dBm
carrier entirely while still reporting plausible noise, which is what an
unsettled AGC looks like (a strong signal needs a large gain change; the
noise floor needs almost none). If that happens on Sweep, a signal genuinely
40dB over the floor could report 25dB over it and fall below the 35dB
margin -- a lost detection, not just a wrong log line.

Method: run the Heltec beacon at a known candidate and alternate Sweep's
retune mode on one firmware image (BENCH_SWEEP_RETUNE), comparing what each
mode reports in the bins covering the carrier (BENCH_SWEEP_FLOOR).

On the timing confound: BEACON is a 2s pulse train, not a continuous
carrier, so a bin visit only sometimes coincides with a pulse. That would
matter if the two modes had very different detection *opportunity* -- but
they nearly don't, because the pulse's airtime dominates both bin dwells:
overlap ~ (dwell + airtime) / interval is ~37.8% for FULL's ~55ms dwell and
~35.5% for LIGHT's ~10ms at a ~700ms airtime. A large observed difference
is therefore about measurement, not luck. Runs are alternated per repeat so
ambient drift and beacon phase land on both arms equally.

Requires the cardputer-adv-bench image with Serial Control enabled, plus the
Heltec transmitter. The transmitter is always stopped on exit.
"""
import argparse
import json
import pathlib
import statistics
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from bench_harness import (CARD_MARKER, TX_MARKER, Endpoint, ResultWriter,
                           card_status, require_ack)

SWEEP_TERMINAL_TIMEOUT_S = 180.0
STATUS_POLL_INTERVAL_S = 0.25


def run_sweep(card):
    if card_status(card).get("W") == "RUNNING":
        raise RuntimeError("sweep already active")
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


def read_floor_curve(card, bin_count, bins=None):
    """Reads per-bin average RSSI. `bins` limits which ones.

    Reading all 85 costs ~85 serial round-trips per sweep and dominated the
    run time badly enough to blow the harness timeout; only the bins around
    the carrier plus a floor sample are actually used, so the default caller
    passes a subset.
    """
    curve = {}
    for b in (range(bin_count) if bins is None else bins):
        opcode, payload = card.request("BENCH_SWEEP_FLOOR", str(b), timeout=6.0)
        if opcode != "ACK":
            raise RuntimeError(f"floor readback failed at bin {b}: {opcode} {payload}")
        fields = dict(item.split("=", 1) for item in payload.split(";"))
        curve[b] = int(fields["FLOOR"]) / 10.0
    return curve


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cardputer-port", default="/dev/ttyACM0")
    ap.add_argument("--heltec-port", default="/dev/ttyACM1")
    ap.add_argument("--candidate", default="LONG_MODERATE")
    ap.add_argument("--carrier-mhz", type=float, default=912.8125)
    ap.add_argument("--repeats", type=int, default=4, help="FULL/LIGHT pairs")
    ap.add_argument("--settle-ms", type=int, default=None,
                    help="BENCH_SWEEP_SETTLE for the LIGHT arm. 0 reproduces the "
                         "pre-fix behaviour M6 measured; omit to leave the "
                         "firmware default in place.")
    ap.add_argument("--margin-dbm-x10", type=int, default=500,
                    help="BENCH_SWEEP_MARGIN for the run. Defaults to the 50dB "
                         "maximum to suppress Pass-A peaks entirely: a peak fires "
                         "Pass-B, and Pass-B sets needsFullRetune, which silently "
                         "gives the NEXT bin a full begin() even in LIGHT mode. "
                         "Without this the LIGHT arm is contaminated by exactly the "
                         "retune it is supposed to exclude.")
    ap.add_argument("--log", required=True)
    ap.add_argument("--results")
    args = ap.parse_args()

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None

    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        tx = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=20.0)
            print("cardputer:", identity)
            if "BENCH=1" not in identity:
                raise RuntimeError(f"not the bench image: {identity}")

            tx = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            print("heltec:", require_ack(tx, "HELLO", "-", timeout=20.0))
            require_ack(tx, "CONFIG", args.candidate, timeout=6.0)
            margin = require_ack(card, "BENCH_SWEEP_MARGIN",
                                 str(args.margin_dbm_x10), timeout=6.0)
            print(f"margin pinned: {margin} (suppresses Pass-B contamination of LIGHT)")
            if args.settle_ms is not None:
                st = require_ack(card, "BENCH_SWEEP_SETTLE", str(args.settle_ms), timeout=6.0)
                print(f"settle pinned: {st}")

            band_lo, step = 902.0, 0.25
            target = int(round((args.carrier_mhz - band_lo) / step))
            window = range(max(0, target - 2), target + 3)
            print(f"carrier {args.carrier_mhz}MHz -> bin {target}; "
                  f"reporting bins {window.start}..{window.stop - 1}\n")

            per_mode = {"FULL": [], "LIGHT": []}
            for rep in range(args.repeats):
                # Beacon is capped (~120 pulses / 4 min), so restart it each
                # repeat rather than assume it is still running.
                try:
                    require_ack(tx, "QUIET", "-", timeout=6.0)
                except Exception:
                    pass
                require_ack(tx, "BEACON", "START", timeout=6.0)
                time.sleep(1.0)

                for mode in ("FULL", "LIGHT"):
                    payload = require_ack(card, "BENCH_SWEEP_RETUNE", mode)
                    if f"RETUNE={mode}" not in payload:
                        raise RuntimeError(f"retune mode not applied: {payload}")
                    # Native USB-CDC drops/garbles a frame occasionally
                    # (bench_harness.parse_frames' own note). One bad frame
                    # should cost a datapoint, not the rest of the matrix.
                    try:
                        status = run_sweep(card)
                        total_bins = int(status.get("WN", "0"))
                        # Carrier neighbourhood + an evenly spread floor
                        # sample. The floor is a median over the sample, so
                        # it does not need every bin to be representative.
                        floor_sample = list(range(0, total_bins, max(1, total_bins // 16)))
                        wanted = sorted(set(list(window) + floor_sample))
                        curve = read_floor_curve(card, total_bins, wanted)
                    except (TimeoutError, RuntimeError, ValueError) as exc:
                        print(f"  rep{rep} {mode:<5} SKIPPED: {exc}")
                        if results:
                            results.write({"event": "sweep_error", "mode": mode,
                                           "repeat": rep, "error": str(exc)})
                        continue
                    near = {b: curve[b] for b in window if b in curve}
                    others = [v for b, v in curve.items() if b not in window]
                    rec = {
                        "mode": mode, "repeat": rep,
                        "away_ms": int(status.get("EA", "0")),
                        "peaks": int(status.get("WP", "0")),
                        "carrier_bins_dbm": near,
                        "carrier_max_dbm": max(near.values()) if near else None,
                        "offband_median_dbm": statistics.median(others) if others else None,
                    }
                    rec["carrier_over_floor_db"] = (
                        rec["carrier_max_dbm"] - rec["offband_median_dbm"]
                        if rec["carrier_max_dbm"] is not None else None)
                    per_mode[mode].append(rec)
                    print(f"  rep{rep} {mode:<5} away={rec['away_ms']:>5}ms peaks={rec['peaks']} "
                          f"carrier_max={rec['carrier_max_dbm']:7.1f}dBm "
                          f"offband_med={rec['offband_median_dbm']:7.1f}dBm "
                          f"=> +{rec['carrier_over_floor_db']:.1f}dB over floor")
                    if results:
                        results.write({"event": "sweep", **rec})

            print()
            summary = {"event": "summary", "carrier_mhz": args.carrier_mhz,
                       "candidate": args.candidate, "target_bin": target}
            for mode, runs in per_mode.items():
                over = [r["carrier_over_floor_db"] for r in runs]
                summary[mode] = {
                    "carrier_over_floor_db_mean": statistics.fmean(over),
                    "carrier_over_floor_db_max": max(over),
                    "carrier_max_dbm_best": max(r["carrier_max_dbm"] for r in runs),
                    "offband_median_dbm_mean": statistics.fmean(
                        r["offband_median_dbm"] for r in runs),
                    "total_peaks": sum(r["peaks"] for r in runs),
                }
                m = summary[mode]
                print(f"{mode:<5} carrier over floor: mean {m['carrier_over_floor_db_mean']:5.1f}dB "
                      f"best {m['carrier_over_floor_db_max']:5.1f}dB | "
                      f"strongest {m['carrier_max_dbm_best']:.1f}dBm | "
                      f"floor {m['offband_median_dbm_mean']:.1f}dBm | "
                      f"peaks {m['total_peaks']}")

            d_best = (summary["LIGHT"]["carrier_over_floor_db_max"]
                      - summary["FULL"]["carrier_over_floor_db_max"])
            summary["light_minus_full_best_over_floor_db"] = d_best
            print(f"\nBest-case carrier-over-floor, LIGHT - FULL = {d_best:+.1f} dB")
            print("Negative and large => the light retune suppresses real signals more "
                  "than it suppresses the floor: a detection problem, not just a log-value one.")
            if results:
                results.write(summary)
        finally:
            if tx is not None:
                try:
                    require_ack(tx, "QUIET", "-", timeout=6.0)
                    print("\ntransmitter stopped (QUIET)")
                except Exception as exc:
                    print(f"\n!! could not stop transmitter cleanly: {exc}")
                tx.port.close()
            card.port.close()
            if results:
                results.close()


if __name__ == "__main__":
    main()
