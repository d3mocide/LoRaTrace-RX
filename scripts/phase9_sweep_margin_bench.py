#!/usr/bin/env python3
"""Phase 9 Sweep noise-floor margin calibration bench.

Runs full Sweeps at a matrix of BENCH_SWEEP_MARGIN values (tenths of dB),
both with the Heltec held quiet and with it repeatedly pulsing a known
LongModerate candidate throughout the sweep window, recording the peak
count STATUS's WP field reports for each condition.

Requires the cardputer-adv-bench image: BENCH_SWEEP_MARGIN is a bench-only
opcode production firmware rejects (mirrors BENCH_CAD's existing
production/bench split, src/bench_fault.cpp).

Same fundamental limitation already on record for Phase 8's CAD symNum
calibration (PROGRESS.md): this bench cannot produce a known-quiet RF
control, so a result here is a real, repeatable room-rate characterization
of peak rate vs margin -- evidence for picking a working margin, not a
calibrated false-positive/miss rate. Transport, framing, retries, and
capture live in bench_harness; this module is scenario policy only.
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
)


# LongModerate sits at 912.8125MHz -- well inside the Sweep engine's own
# 868-923MHz range and already the fixed candidate Phase 8's own bench
# tooling uses, so a positive result here is directly comparable to that
# existing evidence.
PULSE_CANDIDATE = "LONG_MODERATE"
SWEEP_TERMINAL_TIMEOUT_S = 120.0
STATUS_POLL_INTERVAL_S = 0.3


def run_sweep(card: Endpoint, transmitter: Endpoint, pulsing: bool,
              pulse_interval_s: float, arm_delay_ms: int):
    before = card_status(card)
    if before.get("W") == "RUNNING":
        raise RuntimeError(f"Sweep already active before start: {before}")
    home_frequency = before.get("F")

    require_ack(card, "SWEEP_START", "-")
    deadline = time.monotonic() + SWEEP_TERMINAL_TIMEOUT_S
    last_pulse = 0.0
    status = None
    while time.monotonic() < deadline:
        if pulsing and time.monotonic() - last_pulse >= pulse_interval_s:
            try:
                require_ack(transmitter, "ARM", str(arm_delay_ms), timeout=2.0)
            except (RuntimeError, TimeoutError):
                # A transient miss here just means one fewer pulse across a
                # ~60s sweep; it must not abort the whole matrix run.
                pass
            last_pulse = time.monotonic()
        status = card_status(card)
        if status.get("W") in {"COMPLETE", "CANCELLED", "FAILED"}:
            break
        time.sleep(STATUS_POLL_INTERVAL_S)
    else:
        raise TimeoutError(f"Sweep did not reach a terminal state within {SWEEP_TERMINAL_TIMEOUT_S}s")

    if status.get("W") != "COMPLETE":
        raise RuntimeError(f"Sweep ended {status.get('W')}, not COMPLETE: {status}")
    if status.get("F") != home_frequency:
        raise RuntimeError(
            f"Watch did not return to its pre-sweep home frequency {home_frequency}: {status}"
        )
    return status


def run_margin(card: Endpoint, transmitter: Endpoint, margin_dbm_x10: int,
                pulse_interval_s: float, arm_delay_ms: int, repeats: int):
    require_ack(card, "BENCH_SWEEP_MARGIN", str(margin_dbm_x10))

    quiet_trials = []
    active_trials = []
    bins = 0
    for _ in range(repeats):
        quiet = run_sweep(card, transmitter, pulsing=False,
                           pulse_interval_s=pulse_interval_s, arm_delay_ms=arm_delay_ms)
        active = run_sweep(card, transmitter, pulsing=True,
                            pulse_interval_s=pulse_interval_s, arm_delay_ms=arm_delay_ms)
        bins = int(quiet.get("WN", "0"))
        quiet_trials.append(int(quiet.get("WP", "0")))
        active_trials.append(int(active.get("WP", "0")))
    require_ack(transmitter, "QUIET", "-")

    return {
        "margin_dbm_x10": margin_dbm_x10,
        "bins": bins,
        "quiet_peaks_trials": quiet_trials,
        "active_peaks_trials": active_trials,
        "quiet_peaks_mean": sum(quiet_trials) / len(quiet_trials),
        "active_peaks_mean": sum(active_trials) / len(active_trials),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw-control capture")
    parser.add_argument("--results", help="append-only JSONL scenario results")
    parser.add_argument("--margins", default="50,100,150,200,250,300",
                        help="comma-separated tenths-of-dB margin values to test")
    parser.add_argument("--pulse-interval-s", type=float, default=2.0,
                        help="re-arm the Heltec this often during an active-condition sweep")
    parser.add_argument("--arm-delay-ms", type=int, default=250,
                        help="delay from candidate observation to TX for each pulse")
    parser.add_argument("--repeats", type=int, default=1,
                        help="quiet+active trial pairs per margin, for a mean rather than one sample")
    args = parser.parse_args()

    margins = [int(m) for m in args.margins.split(",") if m.strip() != ""]
    for margin in margins:
        if not 0 <= margin <= 500:
            parser.error(f"margin {margin} out of bounds [0, 500] (0-50dB)")
    if not 0.5 <= args.pulse_interval_s <= 30.0:
        parser.error("--pulse-interval-s must be 0.5..30.0")
    if not 10 <= args.arm_delay_ms <= 5000:
        parser.error("--arm-delay-ms must be 10..5000")
    if not 1 <= args.repeats <= 20:
        parser.error("--repeats must be 1..20")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            boot_identity = require_ack(card, "HELLO", "-", timeout=15.0)
            card.record("BOOT_CONFIRMED " + boot_identity)
            if "BENCH=1" not in boot_identity:
                raise RuntimeError(
                    f"Cardputer is not running the bench image (BENCH_SWEEP_MARGIN needs it): {boot_identity}"
                )
            if results:
                results.write({"event": "boot", "identity": boot_identity})

            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", PULSE_CANDIDATE)

            for margin in margins:
                row = run_margin(card, transmitter, margin, args.pulse_interval_s,
                                  args.arm_delay_ms, args.repeats)
                delta = row["active_peaks_mean"] - row["quiet_peaks_mean"]
                print(f"margin={margin/10:.1f}dB bins={row['bins']} repeats={args.repeats} "
                      f"quiet_peaks_mean={row['quiet_peaks_mean']:.1f} "
                      f"active_peaks_mean={row['active_peaks_mean']:.1f} delta={delta:+.1f} "
                      f"quiet_trials={row['quiet_peaks_trials']} active_trials={row['active_peaks_trials']}")
                if results:
                    results.write({"event": "margin_result", **row})

            if results:
                results.write({"event": "complete", "margins_tested": margins})
        finally:
            if transmitter is not None:
                require_ack(transmitter, "QUIET", "-")
                transmitter.close()
            card.close()
            if results:
                results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase9 sweep margin bench: {error}", file=sys.stderr)
        sys.exit(2)
