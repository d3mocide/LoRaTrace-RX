#!/usr/bin/env python3
"""Run one-shot, named Probe fault hooks on a bench Cardputer image."""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack, wait_for


POINTS = {
    "BEFORE_RETUNE", "AFTER_RETUNE", "CAD_WAIT", "RX_WAIT",
    "HOME_RESTORE_BEFORE", "HOME_RESTORE_AFTER",
}
ACTIONS = {"CANCEL", "FAIL"}


def run_cycle(card: Endpoint, transmitter: Endpoint | None, results: ResultWriter | None,
              cycle: int, point: str, action: str):
    before = card_status(card)
    home_frequency = before.get("F")
    if not home_frequency:
        raise RuntimeError(f"Cardputer STATUS omitted home frequency: {before}")
    if before.get("B") == "RUNNING":
        raise RuntimeError("Cardputer Probe is already running")

    require_ack(card, "BENCH_FAULT", f"{point}:{action}")
    # LongModerate is the first non-home candidate. Start its physical pulse
    # before queueing Probe so RX_WAIT is exercised rather than occasionally
    # missed by a late one-shot transmitter command.
    if point == "RX_WAIT" and transmitter is not None:
        require_ack(transmitter, "ARM", "0")
    card.send("PROBE_START", "-")
    terminal = wait_for(
        card,
        lambda status: status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"},
        30.0,
        "faulted Probe terminal state",
    )
    expected = "CANCELLED" if action == "CANCEL" else "FAILED"
    if terminal.get("B") != expected:
        raise RuntimeError(f"cycle {cycle}: expected {expected}, got {terminal}")
    if terminal.get("F") != home_frequency:
        raise RuntimeError(f"cycle {cycle}: home frequency was not restored: {terminal}")
    if int(terminal.get("R", "0")) <= int(before.get("R", "0")):
        raise RuntimeError(f"cycle {cycle}: recovery did not advance: {terminal}")
    if results:
        results.write({"cycle": cycle, "event": "terminal", "point": point,
                       "action": action, "status": terminal})
    return terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", help="optional V4R8 port; required to exercise RX_WAIT")
    parser.add_argument("--log", required=True)
    parser.add_argument("--results")
    parser.add_argument("--point", required=True, choices=sorted(POINTS))
    parser.add_argument("--action", required=True, choices=sorted(ACTIONS))
    parser.add_argument("--cycles", type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            if "BENCH=1" not in identity:
                raise RuntimeError(f"bench image required; HELLO was {identity}")
            card.record("BOOT_CONFIRMED " + identity)
            if results:
                results.write({"event": "boot", "identity": identity,
                               "point": args.point, "action": args.action})
            if args.point == "RX_WAIT" and not args.heltec_port:
                raise RuntimeError("--heltec-port is required for RX_WAIT fault hooks")
            if args.heltec_port:
                transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
                require_ack(transmitter, "HELLO", "-")
                require_ack(transmitter, "CONFIG", "LONG_MODERATE")
            for cycle in range(1, args.cycles + 1):
                terminal = run_cycle(card, transmitter, results, cycle, args.point, args.action)
                print(f"cycle {cycle}/{args.cycles}: {terminal}")
            if results:
                results.write({"event": "complete", "cycles": args.cycles})
        finally:
            if transmitter is not None:
                try:
                    require_ack(transmitter, "QUIET", "-")
                except (RuntimeError, TimeoutError):
                    pass
                transmitter.close()
            card.close()
            if results:
                results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase8 fault bench: {error}", file=sys.stderr)
        sys.exit(2)
