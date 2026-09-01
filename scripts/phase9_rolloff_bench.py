#!/usr/bin/env python3
"""Phase 9 Sweep 923MHz-edge front-end rolloff characterization bench.

Runs one real Sweep on the connected Cardputer, then queries the new
BENCH_SWEEP_FLOOR opcode for every bin's raw average RSSI (the value Pass A
already computes but normally discards once the peak/no-peak decision is
made -- energy.csv only ever persists threshold-filtered peaks). Compares
the mean floor in the top N MHz near the band's own 923MHz ceiling against
the rest of the band to see whether the front end reads a systematically
different floor near its tuning edge (ROADMAP.md's Phase 9 blocking
unknown: "923-928MHz front-end rolloff, especially near 923MHz").

This is a floor-only characterization -- no injected carrier, no second
radio required. Requires the cardputer-adv-bench image: BENCH_SWEEP_FLOOR
is a bench-only opcode production firmware rejects.
"""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import CARD_MARKER, Endpoint, ResultWriter, card_status, require_ack


def run_sweep(card: Endpoint, timeout_s: float):
    before = card_status(card)
    if before.get("W") == "RUNNING":
        raise RuntimeError(f"Sweep already active before start: {before}")
    home_frequency = before.get("F")

    require_ack(card, "SWEEP_START", "-")
    deadline = time.monotonic() + timeout_s
    status = None
    while time.monotonic() < deadline:
        status = card_status(card)
        if status.get("W") in {"COMPLETE", "CANCELLED", "FAILED"}:
            break
        time.sleep(0.3)
    else:
        raise TimeoutError(f"Sweep did not reach a terminal state within {timeout_s}s")

    if status.get("W") != "COMPLETE":
        raise RuntimeError(f"Sweep ended {status.get('W')}, not COMPLETE: {status}")
    if status.get("F") != home_frequency:
        raise RuntimeError(
            f"Watch did not return to its pre-sweep home frequency {home_frequency}: {status}"
        )
    return status


def query_floor(card: Endpoint, bin_index: int):
    opcode, payload = card.request("BENCH_SWEEP_FLOOR", str(bin_index), timeout=6.0)
    if opcode != "ACK":
        return None
    fields = {}
    for item in payload.split(";"):
        key, sep, value = item.partition("=")
        if sep:
            fields[key] = value
    if "FLOOR" not in fields:
        return None
    return int(fields["FLOOR"])  # tenths of dBm


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw-control capture")
    parser.add_argument("--results", help="append-only JSONL per-bin results")
    parser.add_argument("--edge-mhz", type=float, default=5.0,
                        help="width of the near-923MHz edge band to compare, in MHz")
    parser.add_argument("--bin-step-khz", type=float, default=250.0,
                        help="must match ENERGY_SWEEP_DEFAULT_STEP")
    parser.add_argument("--sweep-timeout-s", type=float, default=60.0)
    args = parser.parse_args()

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None

    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        try:
            boot_identity = require_ack(card, "HELLO", "-", timeout=15.0)
            card.record("BOOT_CONFIRMED " + boot_identity)
            if "BENCH=1" not in boot_identity:
                raise RuntimeError(
                    f"Cardputer is not running the bench image (BENCH_SWEEP_FLOOR needs it): {boot_identity}"
                )
            if results:
                results.write({"event": "boot", "identity": boot_identity})

            status = run_sweep(card, args.sweep_timeout_s)
            total_bins = int(status.get("WN", "0"))
            home_freq_khz = float(status.get("F", "0"))
            card.record(f"SWEEP_COMPLETE bins={total_bins} home_freq_khz={home_freq_khz}")

            rows = []
            for bin_index in range(total_bins):
                floor = query_floor(card, bin_index)
                if floor is None:
                    card.record(f"MISSING bin={bin_index}")
                    continue
                rows.append((bin_index, floor))
                if results:
                    results.write({"event": "bin", "bin": bin_index, "floor_dbm_x10": floor})

            if not rows:
                raise RuntimeError("No bins returned floor data -- nothing to analyze")

            # Reconstruct frequency from bin index using the sweep's own
            # step; the band's hi edge is always 923MHz regardless of
            # region (energy_plan.h: only the low edge moves for US).
            hi_mhz = 923.0
            edge_lo_mhz = hi_mhz - args.edge_mhz
            step_mhz = args.bin_step_khz / 1000.0
            max_bin = max(b for b, _ in rows)
            lo_mhz = hi_mhz - max_bin * step_mhz

            def freq_for_bin(b):
                return lo_mhz + b * step_mhz

            edge_vals = [f / 10.0 for b, f in rows if freq_for_bin(b) >= edge_lo_mhz]
            mid_vals = [f / 10.0 for b, f in rows if freq_for_bin(b) < edge_lo_mhz]

            def mean(vals):
                return sum(vals) / len(vals) if vals else float("nan")

            def stdev(vals, m):
                if len(vals) < 2:
                    return float("nan")
                return (sum((v - m) ** 2 for v in vals) / (len(vals) - 1)) ** 0.5

            edge_mean = mean(edge_vals)
            mid_mean = mean(mid_vals)
            edge_sd = stdev(edge_vals, edge_mean)
            mid_sd = stdev(mid_vals, mid_mean)

            print(f"band: {lo_mhz:.3f}-{hi_mhz:.3f}MHz, {len(rows)} bins, step {step_mhz*1000:.0f}kHz")
            print(f"mid-band ({lo_mhz:.3f}-{edge_lo_mhz:.3f}MHz, n={len(mid_vals)}): "
                  f"mean={mid_mean:.2f}dBm sd={mid_sd:.2f}dB")
            print(f"near-923 edge ({edge_lo_mhz:.3f}-{hi_mhz:.3f}MHz, n={len(edge_vals)}): "
                  f"mean={edge_mean:.2f}dBm sd={edge_sd:.2f}dB")
            print(f"edge-vs-mid delta: {edge_mean - mid_mean:+.2f}dB "
                  "(negative = edge reads quieter, consistent with rolloff)")

            if results:
                results.write({
                    "event": "summary",
                    "lo_mhz": lo_mhz, "hi_mhz": hi_mhz, "edge_lo_mhz": edge_lo_mhz,
                    "mid_mean_dbm": mid_mean, "mid_sd_db": mid_sd, "mid_n": len(mid_vals),
                    "edge_mean_dbm": edge_mean, "edge_sd_db": edge_sd, "edge_n": len(edge_vals),
                    "delta_db": edge_mean - mid_mean,
                })
        finally:
            card.close()
            if results:
                results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase9 rolloff bench: {error}", file=sys.stderr)
        sys.exit(2)
