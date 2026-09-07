#!/usr/bin/env python3
"""Workstream 17: does Pass A flag bins that are actually carrying traffic?

Fully passive. The sources are real repeaters on known channels, so this
transmits nothing -- which is both correct etiquette on a live mesh and a more
realistic test than a fixture whose presence during a 3 ms bin visit cannot be
guaranteed. The bench transmitter is explicitly quieted and never armed.

The design needs no external ground truth. Two bins are known to carry traffic
and the rest are not, so the quiet bins are the control *within the same lap*:

  bin 34   MeshCore default, 910.525 MHz  (reported as the busier of the two)
  bin 66   MeshOregon,       918.5 MHz
  others   controls, sampled across the band

If Pass A sees traffic, 34 and 66 should be elevated relative to the controls
more often than the controls are relative to each other. If they are not
distinguishable, that is an answer too.

Settle alternates 0 ms (shipped) and 3 ms (the value the floor measurement
found sufficient) lap by lap, so both configurations see the same traffic
rather than consecutive blocks that could differ in activity. That makes this
a direct test of whether the settle fix improves real detection, not just the
noise floor.
"""

import argparse
import pathlib
import statistics
import sys
import time

import serial

from bench_harness import (CARD_MARKER, TX_MARKER, Endpoint, ResultWriter,
                           card_status, parse_fields, require_ack)

SWEEP_TERMINAL_TIMEOUT_S = 120.0
TRAFFIC_BINS = (34, 66)
# Immediate neighbours, to show whether energy lands only in the intended bin.
ADJACENT_BINS = (33, 35, 65, 67)
CONTROL_BINS = (5, 15, 25, 45, 55, 75, 80)


def run_lap(card):
    require_ack(card, "SWEEP_START", "-", timeout=12.0)
    deadline = time.monotonic() + SWEEP_TERMINAL_TIMEOUT_S
    while time.monotonic() < deadline:
        state = card_status(card).get("W")
        if state in {"COMPLETE", "CANCELLED", "FAILED"}:
            if state != "COMPLETE":
                raise RuntimeError(f"sweep ended {state}")
            return
        time.sleep(0.05)
    raise TimeoutError("sweep did not reach a terminal state")


def read_bin(card, index, attempts=3):
    """Read one bin's floor, retrying transport failures.

    Native USB-CDC occasionally truncates a reply under sustained round trips;
    an earlier run of this bench died at lap 68 of 200 for exactly that. The
    query is idempotent -- it reads state the sweep already recorded -- so a
    re-send cannot disturb the measurement.
    """
    last = None
    for attempt in range(attempts):
        try:
            opcode, payload = card.request("BENCH_SWEEP_FLOOR", str(index), timeout=12.0)
            if opcode != "ACK":
                raise RuntimeError(f"BENCH_SWEEP_FLOOR({index}): {opcode} {payload}")
            return int(parse_fields(payload)["FLOOR"]) / 10.0
        except (RuntimeError, TimeoutError, KeyError, ValueError) as error:
            last = error
            time.sleep(0.3)
    raise RuntimeError(f"BENCH_SWEEP_FLOOR({index}) failed {attempts}x: {last}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", help="quieted if given; never armed")
    parser.add_argument("--bridge-token", default="",
                        help="session token for a networked (socket://) transmitter bridge; printed on that fixture's USB console at bridge start")
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--laps", type=int, default=120, help="total laps, alternating settle")
    parser.add_argument("--settles-ms", type=int, action="append",
                        help="repeatable; default 0 and 3, alternated lap by lap")
    args = parser.parse_args()
    settles = args.settles_ms or [0, 3]
    watched = sorted(set(TRAFFIC_BINS + ADJACENT_BINS + CONTROL_BINS))

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results)
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=45.0)
            if "BENCH=1" not in identity:
                raise RuntimeError(f"needs the bench image: {identity}")
            if args.heltec_port:
                # Not a source here. Quieted explicitly so a leftover beacon
                # from earlier work cannot be mistaken for repeater traffic.
                transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
                # A networked bridge refuses transmit commands until authorized
                # (audit A16); a serial fixture needs no token.
                transmitter.authorize(args.bridge_token)
                require_ack(transmitter, "HELLO", "-", timeout=25.0)
                require_ack(transmitter, "QUIET", "-", timeout=8.0)
                print("bench transmitter quieted; this run transmits nothing")
            require_ack(card, "BENCH_SWEEP_RETUNE", "LIGHT", timeout=8.0)
            results.write({"event": "boot", "identity": identity, "settles_ms": settles,
                           "traffic_bins": list(TRAFFIC_BINS), "adjacent": list(ADJACENT_BINS),
                           "controls": list(CONTROL_BINS), "laps": args.laps})

            started = time.monotonic()
            skipped = 0
            for lap in range(args.laps):
                settle = settles[lap % len(settles)]
                require_ack(card, "BENCH_SWEEP_SETTLE", str(settle), timeout=8.0)
                # A lost lap is a lost sample, not a lost run: at a ~6% hit
                # rate the value is in the count of laps, so aborting on one
                # transport failure discards everything gathered so far.
                try:
                    run_lap(card)
                    floors = {b: read_bin(card, b) for b in watched}
                except (RuntimeError, TimeoutError) as error:
                    skipped += 1
                    results.write({"event": "lap_error", "lap": lap,
                                   "settle_ms": settle, "error": str(error)})
                    print(f"  [{lap + 1:4d}/{args.laps}] skipped: {error}", flush=True)
                    time.sleep(1.0)
                    continue
                results.write({"event": "lap", "lap": lap, "settle_ms": settle,
                               "floors": {str(b): v for b, v in floors.items()},
                               "elapsed_s": round(time.monotonic() - started, 1)})
                if lap % 10 == 0 or lap == args.laps - 1:
                    ctl = statistics.median([floors[b] for b in CONTROL_BINS])
                    print(f"  [{lap + 1:4d}/{args.laps}] settle {settle}ms  "
                          f"bin34 {floors[34]:7.1f}  bin66 {floors[66]:7.1f}  "
                          f"controls {ctl:7.1f}", flush=True)
            print(f"\n{args.laps} laps in {(time.monotonic() - started) / 60:.1f} min, {skipped} skipped")
        finally:
            try:
                require_ack(card, "BENCH_SWEEP_SETTLE", "0", timeout=8.0)
            finally:
                if transmitter is not None:
                    transmitter.close()
                card.close()
                results.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("interrupted; completed laps are already durable", file=sys.stderr)
        sys.exit(130)
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"ws17 sweep traffic: {error}", file=sys.stderr)
        sys.exit(2)
