#!/usr/bin/env python3
"""Run Probe while flooding Cardputer STATUS over the shared USB control path."""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack
from phase8_bench import TARGET_CANDIDATE_FREQ_KHZ


def run_cycle(card, transmitter, results, cycle, arm_delay_ms, poll_interval):
    before = card_status(card)
    home_frequency = before.get("F")
    if not home_frequency or home_frequency == TARGET_CANDIDATE_FREQ_KHZ:
        raise RuntimeError(f"invalid home status for contention cycle: {before}")
    card.send("PROBE_START", "-")
    time.sleep(0.15)
    require_ack(transmitter, "ARM", str(arm_delay_ms))
    deadline = time.monotonic() + 30.0
    polls = 0
    terminal = None
    while time.monotonic() < deadline:
        status = card_status(card)
        polls += 1
        if status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"}:
            terminal = status
            break
        time.sleep(poll_interval)
    if terminal is None:
        raise TimeoutError(f"contention cycle {cycle} did not reach terminal state")
    if terminal.get("B") != "COMPLETE" or terminal.get("F") != home_frequency:
        raise RuntimeError(f"contention cycle {cycle} failed home/terminal check: {terminal}")
    if int(terminal.get("R", "0")) <= int(before.get("R", "0")):
        raise RuntimeError(f"contention cycle {cycle} recovery did not advance: {terminal}")
    if results:
        results.write({"cycle": cycle, "event": "terminal", "status": terminal,
                       "status_polls": polls})
    return terminal, polls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--arm-delay-ms", type=int, default=100)
    parser.add_argument("--poll-interval-ms", type=int, default=25)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")
    if not 5 <= args.poll_interval_ms <= 1000:
        parser.error("--poll-interval-ms must be 5..1000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            card.record("BOOT_CONFIRMED " + identity)
            if results:
                results.write({"event": "boot", "identity": identity})
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", "LONG_MODERATE")
            total_polls = 0
            for cycle in range(1, args.cycles + 1):
                terminal, polls = run_cycle(card, transmitter, results, cycle,
                                             args.arm_delay_ms, args.poll_interval_ms / 1000.0)
                total_polls += polls
                print(f"cycle {cycle}/{args.cycles}: polls={polls} {terminal}")
            require_ack(transmitter, "QUIET", "-")
            if results:
                results.write({"event": "complete", "cycles": args.cycles,
                               "status_polls": total_polls})
        finally:
            if transmitter is not None:
                transmitter.close()
            card.close()
            if results:
                results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase8 contention bench: {error}", file=sys.stderr)
        sys.exit(2)
