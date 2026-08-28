#!/usr/bin/env python3
"""Exercise the nominal Probe cancellation path on a Cardputer.

This scenario intentionally contains no transport implementation. It reuses
``bench_harness`` so cancellation, fault, and contention scenarios share the
same CRC/USB/retry/capture behavior.
"""

import argparse
import pathlib
import sys

import serial

from bench_harness import CARD_MARKER, Endpoint, ResultWriter, card_status, require_ack, wait_for


def run_cycle(card: Endpoint, results: ResultWriter | None, cycle: int):
    before = card_status(card)
    home_frequency = before.get("F")
    if not home_frequency:
        raise RuntimeError(f"Cardputer STATUS omitted home frequency: {before}")
    if before.get("B") == "RUNNING":
        raise RuntimeError("Cardputer Probe is already running")

    card.send("PROBE_START", "-")
    running = wait_for(card, lambda status: status.get("B") == "RUNNING", 15.0, "Probe RUNNING state")
    if results:
        results.write({"cycle": cycle, "event": "running", "status": running})

    cancel_result = require_ack(card, "PROBE_CANCEL", "-")
    if cancel_result != "CANCEL_QUEUED":
        raise RuntimeError(f"cancel was not queued: {cancel_result}")

    terminal = wait_for(
        card,
        lambda status: status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"},
        30.0,
        "Probe cancellation terminal state",
    )
    if terminal.get("B") != "CANCELLED":
        raise RuntimeError(f"cycle {cycle}: expected CANCELLED, got {terminal}")
    if int(terminal.get("R", "0")) <= int(before.get("R", "0")):
        raise RuntimeError(f"cycle {cycle}: home recovery did not advance: {terminal}")
    if terminal.get("F") != home_frequency:
        raise RuntimeError(f"cycle {cycle}: home frequency was not restored: {terminal}")
    if results:
        results.write({"cycle": cycle, "event": "terminal", "status": terminal})
    return terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw-control capture")
    parser.add_argument("--results", help="append-only JSONL scenario results")
    parser.add_argument("--cycles", type=int, default=1, help="cancellation cycles to run")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        try:
            boot_identity = require_ack(card, "HELLO", "-", timeout=15.0)
            card.record("BOOT_CONFIRMED " + boot_identity)
            if results:
                results.write({"event": "boot", "identity": boot_identity})
            for cycle in range(1, args.cycles + 1):
                terminal = run_cycle(card, results, cycle)
                print(f"cycle {cycle}/{args.cycles}: {terminal}")
            if results:
                results.write({"event": "complete", "cycles": args.cycles})
        finally:
            card.close()
            if results:
                results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase8 cancel bench: {error}", file=sys.stderr)
        sys.exit(2)
