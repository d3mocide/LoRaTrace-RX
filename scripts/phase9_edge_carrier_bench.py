#!/usr/bin/env python3
"""Phase 9 Sweep 923MHz-edge injected-carrier characterization bench.

The passive floor-only pass (docs/STATUS.md, 2026-09-01) found no rolloff
signature near the front end's own 923MHz tuning ceiling, but couldn't rule
out reduced *gain* on a real signal -- it never transmitted anything. A
first attempt at an injected-carrier test reused a full Sweep, which failed
for a real reason: a Sweep only dwells on any one bin for ~tens of ms,
while the Heltec's independently-timed pulses essentially never landed
inside that window (docs/STATUS.md has the null result and root cause).

This version fixes that: BENCH_RSSI_WINDOW (bench_fault.h, radio_task.cpp)
parks the Cardputer's radio at one fixed frequency and samples RSSI
continuously for ~2 seconds, so a transmitter firing repeatedly during that
window is very likely to be caught. For a mid-band and an edge-band
candidate (both sourced entries in bench/heltec-v4r8-transmitter's own
CANDIDATES table), this runs a quiet window and a pulsing window and
reports the peak-RSSI rise a real carrier produced at each.

Requires the cardputer-adv-bench image (BENCH_RSSI_WINDOW is bench-only)
and the Heltec transmitter bench sketch.
"""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack

# Sourced from bench/heltec-v4r8-transmitter/src/main.cpp's own CANDIDATES
# table. LONG_MODERATE is the existing mid-band reference
# phase9_sweep_margin_bench.py already uses; SHORT_SLOW sits well inside
# the 918-923MHz edge band the floor-only pass compared against mid-band.
MID_BAND_CANDIDATE = ("LONG_MODERATE", 912.8125)
EDGE_BAND_CANDIDATE = ("SHORT_SLOW", 920.625)

WINDOW_TERMINAL_TIMEOUT_S = 15.0
STATUS_POLL_INTERVAL_S = 0.1


def run_window(card: Endpoint, transmitter: Endpoint, freq_mhz: float, pulsing: bool,
               pulse_interval_s: float, arm_delay_ms: int):
    before = card_status(card)
    if before.get("RW") == "1":
        raise RuntimeError(f"RSSI window already active before start: {before}")

    freq_khz = int(round(freq_mhz * 1000.0))
    require_ack(card, "BENCH_RSSI_WINDOW", str(freq_khz))

    deadline = time.monotonic() + WINDOW_TERMINAL_TIMEOUT_S
    last_pulse = 0.0
    active = True
    while time.monotonic() < deadline:
        if pulsing and time.monotonic() - last_pulse >= pulse_interval_s:
            try:
                require_ack(transmitter, "ARM", str(arm_delay_ms), timeout=2.0)
            except (RuntimeError, TimeoutError):
                pass
            last_pulse = time.monotonic()
        status = card_status(card)
        if status.get("RW") == "0":
            active = False
            break
        time.sleep(STATUS_POLL_INTERVAL_S)
    if active:
        raise TimeoutError(f"RSSI window did not complete within {WINDOW_TERMINAL_TIMEOUT_S}s")

    opcode, payload = card.request("BENCH_RSSI_RESULT", "-", timeout=3.0)
    if opcode != "ACK":
        raise RuntimeError(f"BENCH_RSSI_RESULT failed: {opcode} {payload}")
    fields = {}
    for item in payload.split(";"):
        key, sep, value = item.partition("=")
        if sep:
            fields[key] = value
    if "MAX" not in fields:
        raise RuntimeError(f"BENCH_RSSI_RESULT missing MAX: {payload}")
    return int(fields["MAX"]), int(fields.get("AVG", "0")), int(fields.get("N", "0"))


def run_candidate(card: Endpoint, transmitter: Endpoint, name: str, freq_mhz: float,
                   pulse_interval_s: float, arm_delay_ms: int, repeats: int):
    require_ack(transmitter, "QUIET", "-")
    require_ack(transmitter, "CONFIG", name)

    deltas = []
    quiet_maxes = []
    pulse_maxes = []
    for _ in range(repeats):
        quiet_max, _, quiet_n = run_window(card, transmitter, freq_mhz, pulsing=False,
                                            pulse_interval_s=pulse_interval_s, arm_delay_ms=arm_delay_ms)
        pulse_max, _, pulse_n = run_window(card, transmitter, freq_mhz, pulsing=True,
                                            pulse_interval_s=pulse_interval_s, arm_delay_ms=arm_delay_ms)
        if quiet_n == 0 or pulse_n == 0:
            raise RuntimeError(f"{name}: a window returned zero samples (quiet_n={quiet_n}, pulse_n={pulse_n})")
        quiet_maxes.append(quiet_max)
        pulse_maxes.append(pulse_max)
        deltas.append((pulse_max - quiet_max) / 10.0)  # tenths-of-dB -> dB

    require_ack(transmitter, "QUIET", "-")
    mean_delta = sum(deltas) / len(deltas)
    return {
        "candidate": name,
        "freq_mhz": freq_mhz,
        "quiet_max_dbm_x10": quiet_maxes,
        "pulse_max_dbm_x10": pulse_maxes,
        "deltas_db": deltas,
        "mean_delta_db": mean_delta,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", help="append-only JSONL scenario results")
    parser.add_argument("--pulse-interval-s", type=float, default=0.3,
                        help="re-arm the Heltec this often during a pulsing window "
                             "(much shorter than the margin bench's 2.0s default: the "
                             "window itself is only ~2s, so this needs several pulses "
                             "inside it, not one)")
    parser.add_argument("--arm-delay-ms", type=int, default=100)
    parser.add_argument("--repeats", type=int, default=3,
                        help="quiet+pulse trial pairs per candidate")
    args = parser.parse_args()

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
                    f"Cardputer is not running the bench image (BENCH_RSSI_WINDOW needs it): {boot_identity}"
                )
            if results:
                results.write({"event": "boot", "identity": boot_identity})

            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")

            rows = []
            for name, freq_mhz in (MID_BAND_CANDIDATE, EDGE_BAND_CANDIDATE):
                row = run_candidate(card, transmitter, name, freq_mhz,
                                     args.pulse_interval_s, args.arm_delay_ms, args.repeats)
                rows.append(row)
                print(f"{row['candidate']} ({row['freq_mhz']}MHz): "
                      f"deltas={[f'{d:+.1f}' for d in row['deltas_db']]} "
                      f"mean={row['mean_delta_db']:+.2f}dB")
                if results:
                    results.write({"event": "candidate_result", **row})

            mid, edge = rows[0], rows[1]
            gap = mid["mean_delta_db"] - edge["mean_delta_db"]
            print(f"\nmid-band rise: {mid['mean_delta_db']:+.2f}dB, "
                  f"edge-band rise: {edge['mean_delta_db']:+.2f}dB, "
                  f"gap: {gap:+.2f}dB "
                  "(positive = edge registered a real carrier less strongly than mid-band, "
                  "consistent with reduced front-end gain near 923MHz)")
            if results:
                results.write({"event": "summary", "mid_delta_db": mid["mean_delta_db"],
                                "edge_delta_db": edge["mean_delta_db"], "gap_db": gap})
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
        print(f"phase9 edge carrier bench: {error}", file=sys.stderr)
        sys.exit(2)
