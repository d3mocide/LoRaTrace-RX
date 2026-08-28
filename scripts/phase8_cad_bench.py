#!/usr/bin/env python3
"""Measure bounded Probe CAD behavior with a quiet or pulsed Heltec fixture.

The result is deliberately a measurement rather than a guessed RF verdict:
quiet runs characterize non-fixture CAD activity, while pulsed runs report
misses against the fixture's own TX_DONE evidence. Every run must still
complete, retain SD, and restore the pre-test Watch tuple.
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
    parse_fields,
    require_ack,
    wait_for,
)


TARGETS = {
    "MESH_OREGON": 0x0001,    # Operator-requested tuple is plan index 0.
    "LONG_MODERATE": 0x0004,  # Custom then LongFast precede LongModerate.
}


def cad_counts(status):
    try:
        values = tuple(int(value) for value in status["C"].split(","))
    except (KeyError, ValueError) as error:
        raise RuntimeError(f"Cardputer STATUS omitted valid CAD counts: {status}") from error
    if len(values) != 4:
        raise RuntimeError(f"Cardputer STATUS has malformed CAD counts: {status}")
    return values


def run_cycle(card, transmitter, mode, arm_delay_ms, target_name, target_mask, cycle,
              require_target=True):
    before = card_status(card)
    if before.get("B") == "RUNNING":
        raise RuntimeError("Cardputer Probe is already running")
    home_frequency = before.get("F")
    if not home_frequency or before.get("SD") != "1":
        raise RuntimeError(f"Cardputer is not ready for durable Probe: {before}")
    recovery_before = int(before.get("R", "0"))
    tx_done_before = sum(1 for _, opcode, _ in transmitter.observed if opcode == "TX_DONE")

    if mode == "pulse":
        # Arm before queueing Probe. Native USB delivery is too variable to
        # target a candidate after PROBE_START; a pre-armed fixture gives its
        # preamble a deterministic overlap with the requested candidate.
        require_ack(transmitter, "ARM", str(arm_delay_ms))
    card.send("PROBE_START", "-")

    terminal = wait_for(
        card,
        lambda status: status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"},
        30.0,
        "Probe terminal state",
    )
    if terminal.get("B") != "COMPLETE":
        raise RuntimeError(f"cycle {cycle}: Probe did not complete: {terminal}")
    if terminal.get("F") != home_frequency:
        raise RuntimeError(f"cycle {cycle}: home tuple was not restored: {terminal}")
    if terminal.get("SD") != "1":
        raise RuntimeError(f"cycle {cycle}: SD did not remain ready: {terminal}")
    if int(terminal.get("R", "0")) != recovery_before + 1:
        raise RuntimeError(f"cycle {cycle}: home recovery did not advance once: {terminal}")

    # STATUS drains asynchronous TX_STARTED/TX_DONE lines before the next
    # cycle, making the fixture's own completion evidence per-cycle.
    tx_opcode, tx_payload = transmitter.request("STATUS", "-")
    if tx_opcode != "STATUS":
        raise RuntimeError(f"Heltec STATUS failed: {tx_opcode} {tx_payload}")
    tx_status = parse_fields(tx_payload)
    tx_done_after = sum(1 for _, opcode, _ in transmitter.observed if opcode == "TX_DONE")
    if mode == "pulse" and tx_done_after != tx_done_before + 1:
        raise RuntimeError(f"cycle {cycle}: fixture did not report exactly one TX_DONE")
    if mode == "quiet" and tx_done_after != tx_done_before:
        raise RuntimeError(f"cycle {cycle}: quiet fixture transmitted unexpectedly")

    free, detected, timeout, error = cad_counts(terminal)
    try:
        detected_mask = int(terminal["M"], 16)
    except (KeyError, ValueError) as error:
        raise RuntimeError(f"Cardputer STATUS omitted valid CAD detection mask: {terminal}") from error
    if mode == "pulse" and require_target and not detected_mask & target_mask:
        raise RuntimeError(f"cycle {cycle}: missed controlled {target_name} pulse: {terminal}")
    return {
        "cycle": cycle,
        "mode": mode,
        "terminal": terminal,
        "cad_free": free,
        "cad_detected": detected,
        "cad_timeout": timeout,
        "radio_error": error,
        "cad_detected_mask": f"{detected_mask:04X}",
        "target_bit_detected": bool(detected_mask & target_mask),
        "fixture": tx_status,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--mode", choices=("quiet", "pulse"), required=True)
    parser.add_argument("--candidate", choices=tuple(TARGETS), default="LONG_MODERATE")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--arm-delay-ms", type=int, default=100)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100:
        parser.error("--cycles must be 1..100")
    if not 0 <= args.arm_delay_ms <= 5000:
        parser.error("--arm-delay-ms must be 0..5000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log:
        results = ResultWriter(args.results)
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            card.record("BOOT_CONFIRMED " + identity)
            results.write({"event": "boot", "identity": identity, "mode": args.mode,
                           "candidate": args.candidate})
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", args.candidate)
            require_ack(transmitter, "QUIET", "-")
            for cycle in range(1, args.cycles + 1):
                result = run_cycle(card, transmitter, args.mode, args.arm_delay_ms,
                                   args.candidate, TARGETS[args.candidate], cycle)
                results.write(result)
                print(result)
            results.write({"event": "complete", "mode": args.mode, "cycles": args.cycles})
        finally:
            if transmitter is not None:
                try:
                    require_ack(transmitter, "QUIET", "-")
                except (OSError, RuntimeError, TimeoutError):
                    pass
                transmitter.close()
            card.close()
            results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase8 CAD bench: {error}", file=sys.stderr)
        sys.exit(2)
