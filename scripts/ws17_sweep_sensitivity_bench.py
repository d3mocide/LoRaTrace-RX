#!/usr/bin/env python3
"""Workstream 17: can Pass A flag a bin that is carrying traffic?

v1.0.3 already settled that a full-band sweep cannot *decode* a packet -- that
needs the receiver parked for the whole airtime, which no 85-bin lap can offer,
and increasing samples per bin was tried and reverted on that reasoning.

This asks a different question. Detecting a packet as *energy* needs only that
the bin's short sampling window overlaps the transmission, not that the whole
airtime is captured. Pass A visits each bin for about 3 ms (4 samples, 1 ms
apart) once per ~833 ms lap, so for a 148 ms packet on that bin the geometry
predicts an overlap chance near (148 + 3) / 833, roughly 18%. That is a
falsifiable number, and it is far from the "fraction of a percent" that 3 ms of
833 ms suggests at a glance.

Three arms per run:

  persistent  the transmitter radiates continuously. A control on the
              instrument: if Pass A cannot flag the bin here, the sparse arm
              measures nothing and the threshold margin is the story.
  sparse      one packet per interval, so the overlap is a lottery with known
              odds. This is the measurement.
  silent      no transmission. Establishes the bin's own floor distribution,
              which is what "flagged" has to be judged against.

Reports the per-lap flag rate for each arm with Wilson intervals, against the
geometric prediction. It selects no threshold and changes no firmware.
"""

import argparse
import json
import math
import pathlib
import statistics
import sys
import threading
import time

import serial

from bench_harness import (CARD_MARKER, TX_MARKER, Endpoint, ResultWriter,
                           card_status, parse_fields, require_ack)


BAND_HI_MHZ = 923.0   # fixed hardware ceiling regardless of region (energy_plan.h)
BIN_STEP_MHZ = 0.25
SWEEP_TERMINAL_TIMEOUT_S = 90.0
# Measured ARM-to-TX_STARTED latency over the WiFi control bridge.
PIPELINE_PRIME_S = 2.5


def wilson(successes, total, z=1.959963984540054):
    if total == 0:
        return (0.0, 1.0)
    p = successes / total
    denom = 1.0 + z * z / total
    centre = (p + z * z / (2 * total)) / denom
    margin = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denom
    return (max(0.0, centre - margin), min(1.0, centre + margin))


def bin_for_freq(freq_mhz, total_bins):
    lo = BAND_HI_MHZ - (total_bins - 1) * BIN_STEP_MHZ
    return max(0, min(total_bins - 1, round((freq_mhz - lo) / BIN_STEP_MHZ)))


class Firer:
    """Fires the transmitter on its own schedule for the duration of a lap.

    `interval_s` of None means fire as fast as each transmission completes,
    which is the persistent arm. Otherwise one pulse per interval, giving the
    sparse arm its known duty.
    """

    def __init__(self, transmitter, interval_s):
        self.transmitter = transmitter
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thread = None
        self.fired = 0

    def _run(self):
        while not self._stop.is_set():
            try:
                require_ack(self.transmitter, "ARM", "0", timeout=2.0)
                self.fired += 1
            except (RuntimeError, TimeoutError, OSError):
                pass
            self._stop.wait(self.interval_s if self.interval_s else 0.05)

    def __enter__(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=8.0)
        return False


def run_lap(card, target_bin):
    """One full Pass A lap; returns that bin's averaged RSSI in dBm."""
    require_ack(card, "SWEEP_START", "-", timeout=12.0)
    deadline = time.monotonic() + SWEEP_TERMINAL_TIMEOUT_S
    while time.monotonic() < deadline:
        state = card_status(card).get("W")
        if state in {"COMPLETE", "CANCELLED", "FAILED"}:
            if state != "COMPLETE":
                raise RuntimeError(f"sweep ended {state}")
            break
        time.sleep(0.1)
    else:
        raise TimeoutError("sweep did not reach a terminal state")
    opcode, payload = card.request("BENCH_SWEEP_FLOOR", str(target_bin), timeout=8.0)
    if opcode != "ACK":
        raise RuntimeError(f"BENCH_SWEEP_FLOOR failed: {opcode} {payload}")
    # Replies as "BIN=<n>;FLOOR=<tenths of a dBm>".
    fields = parse_fields(payload)
    if "FLOOR" not in fields:
        raise RuntimeError(f"BENCH_SWEEP_FLOOR gave no floor: {payload}")
    return int(fields["FLOOR"]) / 10.0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--bridge-token", default="",
                        help="session token for a networked (socket://) transmitter bridge; printed on that fixture's USB console at bridge start")
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--candidate", default="MESH_OREGON")
    parser.add_argument("--freq-mhz", type=float, default=918.5)
    parser.add_argument("--airtime-ms", type=float, default=148.0,
                        help="measured airtime of the candidate, for the prediction")
    parser.add_argument("--laps", type=int, default=30, help="laps per arm")
    parser.add_argument("--sparse-interval-s", type=float, default=0.8)
    parser.add_argument("--allow-transmit", action="store_true")
    args = parser.parse_args()
    if not args.allow_transmit:
        parser.error("this bench arms the transmitter; pass --allow-transmit")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results)
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=45.0)
            if "BENCH=1" not in identity:
                raise RuntimeError(f"needs the bench image for BENCH_SWEEP_FLOOR: {identity}")
            status = card_status(card)
            total_bins = int(status.get("WN", "0")) or 85
            target = bin_for_freq(args.freq_mhz, total_bins)
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            # A networked bridge refuses transmit commands until authorized
            # (audit A16); a serial fixture needs no token.
            transmitter.authorize(args.bridge_token)
            require_ack(transmitter, "HELLO", "-", timeout=25.0)
            require_ack(transmitter, "QUIET", "-", timeout=8.0)
            require_ack(transmitter, "CONFIG", args.candidate, timeout=10.0)

            print(f"target {args.freq_mhz} MHz -> bin {target} of {total_bins}")
            results.write({"event": "boot", "identity": identity, "target_bin": target,
                           "total_bins": total_bins, "candidate": args.candidate,
                           "freq_mhz": args.freq_mhz, "airtime_ms": args.airtime_ms,
                           "laps": args.laps, "sparse_interval_s": args.sparse_interval_s})

            floors = {}
            for arm, interval in (("silent", None), ("persistent", None),
                                  ("sparse", args.sparse_interval_s)):
                print(f"\n[{arm}] {args.laps} laps")
                values = []
                for lap in range(args.laps):
                    if arm == "silent":
                        fired = 0
                        floor = run_lap(card, target)
                    else:
                        with Firer(transmitter, interval) as firer:
                            # The control bridge adds roughly 1.5 s between ARM
                            # and the transmission actually starting. Firing
                            # only for the lap's duration therefore radiates
                            # entirely after the lap has ended, which reads as
                            # a silent band -- it did, identically across all
                            # three arms, before this wait was added. Prime the
                            # pipeline first so the source is genuinely on air
                            # while Pass A is sampling.
                            time.sleep(PIPELINE_PRIME_S)
                            floor = run_lap(card, target)
                        fired = firer.fired
                    values.append(floor)
                    results.write({"event": "lap", "arm": arm, "lap": lap,
                                   "bin": target, "floor_dbm": floor, "pulses": fired})
                    print(f"  [{lap + 1:3d}/{args.laps}] bin {target} floor {floor:6.1f} dBm"
                          f"  pulses {fired}", flush=True)
                floors[arm] = values
                require_ack(transmitter, "QUIET", "-", timeout=8.0)

            # "Flagged" is judged against the bin's own silent distribution, not
            # an absolute number: Phase 12 rejected absolute thresholds at field
            # levels, and the same objection applies here.
            base = statistics.median(floors["silent"])
            spread = max(floors["silent"]) - base
            threshold = base + max(3.0, spread + 1.0)
            print(f"\nsilent floor median {base:.1f} dBm, max {max(floors['silent']):.1f} dBm")
            print(f"flag threshold: {threshold:.1f} dBm (silent median + margin)\n")
            predicted = (args.airtime_ms + 3.0) / 833.0
            for arm in ("silent", "persistent", "sparse"):
                hits = sum(1 for v in floors[arm] if v >= threshold)
                lo, hi = wilson(hits, len(floors[arm]))
                extra = ""
                if arm == "sparse":
                    extra = f"   geometric prediction ~{predicted:.1%}"
                print(f"  {arm:11s} flagged {hits}/{len(floors[arm])} "
                      f"= {hits / len(floors[arm]):5.1%}  95% CI [{lo:.3f},{hi:.3f}]{extra}")
                results.write({"event": "summary", "arm": arm, "hits": hits,
                               "laps": len(floors[arm]), "threshold_dbm": threshold,
                               "wilson95": [round(lo, 4), round(hi, 4)],
                               "floor_median": statistics.median(floors[arm]),
                               "floor_max": max(floors[arm])})
        finally:
            if transmitter is not None:
                try:
                    require_ack(transmitter, "QUIET", "-", timeout=8.0)
                finally:
                    transmitter.close()
            card.close()
            results.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("interrupted; completed laps are already durable", file=sys.stderr)
        sys.exit(130)
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"ws17 sweep sensitivity: {error}", file=sys.stderr)
        sys.exit(2)
