#!/usr/bin/env python3
"""Measure one bounded Focus bin quietly, then against one controlled pulse.

This is Phase 12 engineering evidence, not a coverage-calibration tool. It
uses the shared framed transport, leaves the Heltec quiet on every exit path,
and requires explicit --allow-transmit before it can arm a pulse. Results
contain only the GPS-free compact Focus summary; `focus.csv` is the durable
device-side evidence.
"""

import argparse
import pathlib
import sys

import serial

from bench_harness import (CARD_MARKER, TX_MARKER, Endpoint, ResultWriter,
                           card_status, parse_fields, require_ack, wait_for)


TX_CANDIDATE = "LONG_MODERATE"  # 912.8125 MHz, SF11/BW125/CR4/8, sync 0x2B


def request_focus(card, bin_index, dwell_ms, samples, prior_status):
    """Request one Focus pass and require its SD commit before readback."""
    before_written = int(prior_status.get("FW", "0"))
    before_enqueued = int(prior_status.get("FO", "0"))
    require_ack(card, "BENCH_FOCUS", f"{bin_index}:{dwell_ms}:{samples}")
    terminal = wait_for(
        card,
        lambda status: status.get("FS") == "3"
        and int(status.get("FO", "0")) > before_enqueued
        and int(status.get("FW", "0")) > before_written,
        12.0,
        "Focus completion, restore, and focus.csv commit",
    )
    result = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "-"))
    if result.get("RS") != "0" or result.get("HR") != "1":
        raise RuntimeError(f"Focus did not complete with home restore: {result}")
    if int(result.get("N", "0")) != samples:
        raise RuntimeError(f"Focus sample count mismatch: expected {samples}, got {result}")
    return terminal, result


def record(writer, event, status, result, **extra):
    if writer is not None:
        writer.write({"event": event, "status": status, "result": result, **extra})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw framed-control log")
    parser.add_argument("--results", help="append-only JSONL summary path")
    parser.add_argument("--bin", type=int, default=43,
                        help="US 250 kHz bin (default 43 = 912.750 MHz)")
    parser.add_argument("--dwell-ms", type=int, default=500)
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--with-pulse", action="store_true", help="also measure one Heltec pulse")
    parser.add_argument("--allow-transmit", action="store_true", help="required with --with-pulse")
    parser.add_argument("--arm-delay-ms", type=int, default=150)
    args = parser.parse_args()
    if args.with_pulse and not args.allow_transmit:
        parser.error("--with-pulse requires explicit --allow-transmit")
    if not 0 <= args.bin <= 65535:
        parser.error("--bin must fit uint16")
    if not 2 <= args.dwell_ms <= 2000:
        parser.error("--dwell-ms must be 2..2000")
    if not 2 <= args.samples <= 64:
        parser.error("--samples must be 2..64")
    if not 10 <= args.arm_delay_ms <= 5000:
        parser.error("--arm-delay-ms must be 10..5000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            require_ack(card, "HELLO", "-", timeout=15.0)
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "QUIET", "-")

            quiet_status, quiet = request_focus(
                card, args.bin, args.dwell_ms, args.samples, card_status(card)
            )
            print(f"quiet: {quiet}")
            record(results, "quiet", quiet_status, quiet)
            if not args.with_pulse:
                return

            require_ack(transmitter, "CONFIG", TX_CANDIDATE)
            on_before = card_status(card)
            require_ack(card, "BENCH_FOCUS", f"{args.bin}:{args.dwell_ms}:{args.samples}")
            require_ack(transmitter, "ARM", str(args.arm_delay_ms))
            on_status = wait_for(
                card,
                lambda status: status.get("FS") == "3"
                and int(status.get("FO", "0")) > int(on_before.get("FO", "0"))
                and int(status.get("FW", "0")) > int(on_before.get("FW", "0")),
                12.0,
                "pulsed Focus completion, restore, and focus.csv commit",
            )
            on = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "-"))
            if on.get("RS") != "0" or on.get("HR") != "1":
                raise RuntimeError(f"pulsed Focus did not restore: {on}")
            if int(on.get("N", "0")) != args.samples:
                raise RuntimeError(f"pulsed Focus sample count mismatch: {on}")
            transmitter.request("STATUS", "-")  # Drains TX_STARTED/TX_DONE evidence.
            events = {opcode for _, opcode, _ in transmitter.observed}
            if not {"TX_STARTED", "TX_DONE"}.issubset(events):
                raise RuntimeError(f"transmitter pulse incomplete: {sorted(events)}")
            delta = {field: int(on[field]) - int(quiet[field]) for field in ("MED", "P90", "MAX")}
            print(f"pulse: {on}; delta_tenths_db={delta}")
            record(results, "pulse", on_status, on, delta_tenths_db=delta,
                   tx_candidate=TX_CANDIDATE)
        finally:
            if transmitter is not None:
                try:
                    require_ack(transmitter, "QUIET", "-", timeout=5.0)
                finally:
                    transmitter.close()
            card.close()
            if results is not None:
                results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase12 Focus bench: {error}", file=sys.stderr)
        sys.exit(2)
