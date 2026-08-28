#!/usr/bin/env python3
"""Measure CAD target-bit quiet and pulse rates for every SX1262 CAD window.

Requires the compile-time-gated ``cardputer-adv-bench`` image. The normal
Cardputer image rejects ``BENCH_CAD`` and always uses the two-symbol setting.
Each cycle is independently required to complete, retain SD, and restore Watch.
"""

import argparse
import pathlib
import sys

import serial

from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack
from phase8_cad_bench import TARGETS, run_cycle


SYMBOL_WINDOWS = (1, 2, 4, 8, 16)
TARGET_FREQUENCIES_KHZ = {
    "MESH_OREGON": "918500",
    "LONG_MODERATE": "912812",  # STATUS truncates 912.8125 MHz to kHz.
}


def parse_windows(text):
    try:
        values = tuple(int(item) for item in text.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("symbols must be comma-separated integers") from error
    if not values or any(value not in SYMBOL_WINDOWS for value in values):
        raise argparse.ArgumentTypeError("symbols must be selected from 1,2,4,8,16")
    if len(set(values)) != len(values):
        raise argparse.ArgumentTypeError("symbols must not contain duplicates")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--candidate", choices=tuple(TARGETS), default="LONG_MODERATE")
    parser.add_argument("--symbols", type=parse_windows, default=SYMBOL_WINDOWS)
    parser.add_argument("--quiet-cycles", type=int, default=20)
    parser.add_argument("--pulse-cycles", type=int, default=20)
    parser.add_argument("--observe-only", action="store_true",
                        help="record every window without failing the process on a rejected rate")
    args = parser.parse_args()
    if not 1 <= args.quiet_cycles <= 100 or not 0 <= args.pulse_cycles <= 100:
        parser.error("quiet cycles must be 1..100 and pulse cycles must be 0..100")
    if args.pulse_cycles == 0 and not args.observe_only:
        parser.error("a quiet-only run is observational; pass --observe-only")

    target_mask = TARGETS[args.candidate]
    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log:
        results = ResultWriter(args.results)
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            if "BENCH=1" not in identity:
                raise RuntimeError("CAD rate matrix requires cardputer-adv-bench firmware")
            before = card_status(card)
            if before.get("F") == TARGET_FREQUENCIES_KHZ[args.candidate]:
                raise RuntimeError("CAD rate target equals the active home channel; choose another candidate")
            card.record("BOOT_CONFIRMED " + identity)
            results.write({"event": "boot", "identity": identity, "candidate": args.candidate,
                           "symbols": args.symbols, "quiet_cycles": args.quiet_cycles,
                           "pulse_cycles": args.pulse_cycles})
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", args.candidate)
            require_ack(transmitter, "QUIET", "-")

            rejected = []
            for symbols in args.symbols:
                require_ack(card, "BENCH_CAD", str(symbols))
                quiet = []
                for cycle in range(1, args.quiet_cycles + 1):
                    result = run_cycle(card, transmitter, "quiet", 0, args.candidate,
                                       target_mask, cycle,
                                       require_target=not args.observe_only)
                    result.update({"event": "cycle", "symbols": symbols})
                    results.write(result)
                    quiet.append(result)
                pulse = []
                for cycle in range(1, args.pulse_cycles + 1):
                    result = run_cycle(card, transmitter, "pulse", 0, args.candidate,
                                       target_mask, cycle,
                                       require_target=not args.observe_only)
                    result.update({"event": "cycle", "symbols": symbols})
                    results.write(result)
                    pulse.append(result)
                quiet_target_hits = sum(item["target_bit_detected"] for item in quiet)
                pulse_target_hits = sum(item["target_bit_detected"] for item in pulse)
                quiet_cad_timeouts = sum(item["cad_timeout"] for item in quiet)
                pulse_cad_timeouts = sum(item["cad_timeout"] for item in pulse)
                pulse_rate = pulse_target_hits / len(pulse) if pulse else None
                accepted = (bool(pulse) and
                            quiet_target_hits == 0 and
                            pulse_target_hits == len(pulse) and
                            quiet_cad_timeouts == 0 and
                            pulse_cad_timeouts == 0)
                summary = {"event": "window_complete", "symbols": symbols,
                           "quiet_cycles": len(quiet), "quiet_target_hits": quiet_target_hits,
                           "pulse_cycles": len(pulse), "pulse_target_hits": pulse_target_hits,
                           "quiet_cad_timeouts": quiet_cad_timeouts,
                           "pulse_cad_timeouts": pulse_cad_timeouts,
                           "quiet_rate": quiet_target_hits / len(quiet),
                           "pulse_rate": pulse_rate,
                           "quiet_only": not pulse,
                           "accepted": accepted}
                results.write(summary)
                print(summary)
                if not summary["accepted"]:
                    rejected.append(summary)
            results.write({"event": "complete", "candidate": args.candidate,
                           "symbols": args.symbols, "rejected_windows": rejected,
                           "accepted": not rejected})
            if rejected and not args.observe_only:
                raise RuntimeError(f"CAD strict gate rejected {len(rejected)} window(s): {rejected}")
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
        print(f"phase8 CAD rate bench: {error}", file=sys.stderr)
        sys.exit(2)
