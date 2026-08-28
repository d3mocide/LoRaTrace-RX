#!/usr/bin/env python3
"""Run the nominal deterministic Probe scenario over two USB serial ports.

Transport, framing, retries, and capture live in :mod:`bench_harness`; this
module contains only the Phase 8 scenario policy. Future cancellation/fault
scenarios should reuse the same shared transport instead of copying it.
"""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import (
    CARD_MARKER,
    TX_MARKER,
    Endpoint,
    ResultWriter,
    card_status,
    require_ack,
    wait_for,
)


TARGET_CANDIDATE_FREQ_KHZ = "912812"  # LongModerate; float status truncates .5 kHz.


def run_cycle(card: Endpoint, transmitter: Endpoint, arm_delay_ms: int, cycle: int):
    before = card_status(card)
    home_frequency = before.get("F")
    if not home_frequency:
        raise RuntimeError(f"Cardputer STATUS omitted home frequency: {before}")
    if home_frequency == TARGET_CANDIDATE_FREQ_KHZ:
        raise RuntimeError("home frequency equals the LongModerate target; choose another candidate")
    if before.get("B") == "RUNNING":
        raise RuntimeError("Cardputer Probe is already running")

    # STATUS/ACK delivery can arrive late on native USB while the radio has
    # already advanced candidates. Send PROBE_START once, then arm without
    # waiting on that asynchronous ACK; the fixed-plan lead places the pulse
    # in the LongFast -> LongModerate window. Terminal STATUS below still
    # proves the Probe ran and restored home, while ARM remains one-shot.
    card.send("PROBE_START", "-")
    time.sleep(0.15)
    require_ack(transmitter, "ARM", str(arm_delay_ms))
    terminal = wait_for(
        card,
        lambda status: status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"},
        30.0,
        "Probe terminal state",
    )
    if int(terminal.get("R", "0")) <= int(before.get("R", "0")):
        raise RuntimeError(f"cycle {cycle}: home recovery did not advance: {terminal}")
    if terminal.get("F") != home_frequency:
        raise RuntimeError(
            f"cycle {cycle}: Watch did not return to its pre-test home frequency "
            f"{home_frequency}: {terminal}"
        )
    return terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw-control capture")
    parser.add_argument("--results", help="append-only JSONL scenario results")
    parser.add_argument("--cycles", type=int, default=1, help="Probe cycles to run (start with 1)")
    parser.add_argument("--arm-delay-ms", type=int, default=100, help="delay from candidate observation to TX")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")
    if not 10 <= args.arm_delay_ms <= 5000:
        parser.error("--arm-delay-ms must be 10..5000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            # Native USB opening resets the Cardputer; its UI task must finish
            # hardware initialization before Low Profile begins polling.
            boot_identity = require_ack(card, "HELLO", "-", timeout=15.0)
            card.record("BOOT_CONFIRMED " + boot_identity)
            if results:
                results.write({"event": "boot", "identity": boot_identity})
            # Do not open the transmitter until the Cardputer has answered at
            # the application protocol layer; opening native USB can reset it.
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", "LONG_MODERATE")
            for cycle in range(1, args.cycles + 1):
                terminal = run_cycle(card, transmitter, args.arm_delay_ms, cycle)
                print(f"cycle {cycle}/{args.cycles}: {terminal}")
                if results:
                    results.write({
                        "cycle": cycle,
                        "total_cycles": args.cycles,
                        "terminal": terminal,
                    })
            require_ack(transmitter, "QUIET", "-")
            tx_events = {opcode for _, opcode, _ in transmitter.observed}
            if "TX_STARTED" not in tx_events or "TX_DONE" not in tx_events:
                raise RuntimeError(f"Heltec did not report a complete pulse: {sorted(tx_events)}")
            if results:
                results.write({"event": "complete", "cycles": args.cycles})
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
        print(f"phase8 bench: {error}", file=sys.stderr)
        sys.exit(2)
