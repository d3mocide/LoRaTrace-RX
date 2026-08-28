#!/usr/bin/env python3
"""Run all named fault points from one stable Cardputer/Heltec boot."""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack, wait_for


CASES = [(point, action) for point in
         ("BEFORE_RETUNE", "AFTER_RETUNE", "CAD_WAIT", "RX_WAIT",
          "HOME_RESTORE_BEFORE", "HOME_RESTORE_AFTER")
         for action in ("CANCEL", "FAIL")]


def run_case(card, transmitter, results, number, point, action):
    before = card_status(card)
    home_frequency = before.get("F")
    if not home_frequency or before.get("B") == "RUNNING":
        raise RuntimeError(f"case {number} invalid baseline: {before}")
    require_ack(card, "BENCH_FAULT", f"{point}:{action}")
    # LongModerate is the first non-home candidate on this fixture. Arm the
    # Heltec *before* queuing Probe so its real packet occupies that first
    # CAD/receive window; arming 150ms afterwards was intermittently too
    # late and left an RX_WAIT hook correctly unconsumed.
    if point == "RX_WAIT":
        require_ack(transmitter, "ARM", "0")
    card.send("PROBE_START", "-")
    terminal = wait_for(card, lambda status: status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"},
                        30.0, f"{point}:{action} terminal state")
    expected = "CANCELLED" if action == "CANCEL" else "FAILED"
    if terminal.get("B") != expected or terminal.get("F") != home_frequency:
        raise RuntimeError(f"case {number} {point}:{action} expected {expected}/home, got {terminal}")
    if int(terminal.get("R", "0")) <= int(before.get("R", "0")):
        raise RuntimeError(f"case {number} {point}:{action} recovery did not advance: {terminal}")
    if results:
        results.write({"case": number, "event": "terminal", "point": point,
                       "action": action, "status": terminal})
    return terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results")
    parser.add_argument("--settle-seconds", type=float, default=1.0,
                        help="quiet interval between cases (default: 1.0)")
    args = parser.parse_args()
    if not 0.0 <= args.settle_seconds <= 10.0:
        parser.error("--settle-seconds must be 0..10")
    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            if "BENCH=1" not in identity:
                raise RuntimeError(f"bench image required; HELLO was {identity}")
            card.record("BOOT_CONFIRMED " + identity)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", "LONG_MODERATE")
            if results:
                results.write({"event": "boot", "identity": identity, "cases": len(CASES)})
            for number, (point, action) in enumerate(CASES, 1):
                terminal = run_case(card, transmitter, results, number, point, action)
                print(f"case {number}/{len(CASES)} {point}:{action}: {terminal}")
                # Native USB can coalesce/backlog several short responses
                # after a rapid run of terminal-state polling. Give the UI's
                # control owner one bounded scheduling window before arming
                # the next case; this is test pacing, not a firmware delay.
                if number < len(CASES) and args.settle_seconds > 0.0:
                    time.sleep(args.settle_seconds)
            require_ack(transmitter, "QUIET", "-")
            if results:
                results.write({"event": "complete", "cases": len(CASES)})
        finally:
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
        print(f"phase8 fault matrix: {error}", file=sys.stderr)
        sys.exit(2)
