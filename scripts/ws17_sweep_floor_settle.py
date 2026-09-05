#!/usr/bin/env python3
"""Workstream 17: how far does Pass A's settle time move its noise floor?

Needs no transmitter. That is the point: the floor is a property of the
receiver and its retune, so measuring it silently removes every source-timing
problem that invalidated the first attempts at the detection question.

Pass A retunes each bin with standby/setFrequency/startReceive and then samples
immediately -- `ENERGY_SWEEP_SETTLE_DEFAULT_MS` is 0. `src/version.h` records
an 11 dB under-read on Cell from exactly that, and leaves Sweep's case open.
A first probe put Sweep's silent floor 14 dB below a settled reading; this
establishes it properly and finds where the reading stabilises.

It also measures two things any fix has to trade against each other:

  lap time     a settle is paid once per bin, so 85 bins multiply it. This is
               the cost side of "make Pass A settle" versus "recalibrate the
               margin against under-read values".
  bin 0        bin 0 takes a full begin(), later bins take the light retune.
               If the under-read is a settle artifact, bin 0 should be
               unaffected while the rest are not -- which localises the cause
               rather than inferring it.

Reports floors per bin per settle value. It selects nothing and changes no
firmware; the shipped default is restored on exit.
"""

import argparse
import pathlib
import statistics
import sys
import time

import serial

from bench_harness import (CARD_MARKER, Endpoint, ResultWriter, card_status,
                           parse_fields, require_ack)

SWEEP_TERMINAL_TIMEOUT_S = 120.0
DEFAULT_SETTLES_MS = (0, 1, 2, 3, 5, 10, 20, 40)
DEFAULT_BINS = (0, 20, 40, 66, 84)


def run_lap(card):
    """One Pass A lap; returns its wall-clock duration in seconds."""
    started = time.monotonic()
    require_ack(card, "SWEEP_START", "-", timeout=12.0)
    deadline = time.monotonic() + SWEEP_TERMINAL_TIMEOUT_S
    while time.monotonic() < deadline:
        state = card_status(card).get("W")
        if state in {"COMPLETE", "CANCELLED", "FAILED"}:
            if state != "COMPLETE":
                raise RuntimeError(f"sweep ended {state}")
            return time.monotonic() - started
        time.sleep(0.05)
    raise TimeoutError("sweep did not reach a terminal state")


def read_bin(card, index):
    opcode, payload = card.request("BENCH_SWEEP_FLOOR", str(index), timeout=8.0)
    if opcode != "ACK":
        raise RuntimeError(f"BENCH_SWEEP_FLOOR({index}) failed: {opcode} {payload}")
    fields = parse_fields(payload)
    return int(fields["FLOOR"]) / 10.0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--laps", type=int, default=5, help="laps per settle value")
    parser.add_argument("--settles-ms", type=int, action="append",
                        help=f"repeatable; default {' '.join(map(str, DEFAULT_SETTLES_MS))}")
    parser.add_argument("--bins", type=int, action="append",
                        help=f"repeatable; default {' '.join(map(str, DEFAULT_BINS))}")
    args = parser.parse_args()
    settles = args.settles_ms or list(DEFAULT_SETTLES_MS)
    bins = args.bins or list(DEFAULT_BINS)

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results)
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        try:
            identity = require_ack(card, "HELLO", "-", timeout=45.0)
            if "BENCH=1" not in identity:
                raise RuntimeError(f"needs the bench image: {identity}")
            results.write({"event": "boot", "identity": identity,
                           "settles_ms": settles, "bins": bins, "laps": args.laps})
            require_ack(card, "BENCH_SWEEP_RETUNE", "LIGHT", timeout=8.0)

            table = {}
            print(f"{'settle':>7s} {'lap':>8s}  " + "  ".join(f"bin{b:<4d}" for b in bins))
            for settle in settles:
                require_ack(card, "BENCH_SWEEP_SETTLE", str(settle), timeout=8.0)
                per_bin = {b: [] for b in bins}
                durations = []
                for _ in range(args.laps):
                    durations.append(run_lap(card))
                    for b in bins:
                        per_bin[b].append(read_bin(card, b))
                med = {b: statistics.median(v) for b, v in per_bin.items()}
                table[settle] = med
                lap_s = statistics.median(durations)
                results.write({"event": "settle", "settle_ms": settle,
                               "lap_seconds": round(lap_s, 3),
                               "floors": {str(b): med[b] for b in bins},
                               "raw": {str(b): per_bin[b] for b in bins}})
                print(f"{settle:5d}ms {lap_s:7.2f}s  "
                      + "  ".join(f"{med[b]:7.1f}" for b in bins), flush=True)

            # bin 0 takes a full begin(); the rest take the light retune. If the
            # under-read is a settle artifact it should show that asymmetry.
            print()
            base = table[max(settles)]
            worst = table[min(settles)]
            print(f"shift from {min(settles)}ms to {max(settles)}ms settle, per bin:")
            for b in bins:
                tag = "  <- full begin() bin" if b == 0 else ""
                print(f"  bin {b:3d}: {worst[b]:7.1f} -> {base[b]:7.1f}  "
                      f"({base[b] - worst[b]:+5.1f} dB){tag}")
        finally:
            # Never leave the bench image on a non-default sweep configuration.
            try:
                require_ack(card, "BENCH_SWEEP_SETTLE", "0", timeout=8.0)
                require_ack(card, "BENCH_SWEEP_RETUNE", "LIGHT", timeout=8.0)
            finally:
                card.close()
                results.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("interrupted; completed settle values are already durable", file=sys.stderr)
        sys.exit(130)
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"ws17 floor/settle: {error}", file=sys.stderr)
        sys.exit(2)
