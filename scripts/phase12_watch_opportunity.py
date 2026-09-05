#!/usr/bin/env python3
"""Phase 12 §6.3: what does interleaving Focus cost Watch packet opportunity?

Two arms against an identical, independently timed pulse train on the home
channel: Watch alone, then Watch with repeated bounded Focus requests. Reports
received/reference for each as a Wilson 95% interval, plus Focus away time,
completed requests, and the gaps between restored Watch windows.

This measures a cost. It does not decide whether that cost is acceptable —
that is an operator product decision after the measurement, and a displayed
away-time does not make the loss free (docs/research/phase12-survey-truth-design.md
§6.3).

The transmitter must be configured to a candidate that matches the receiver's
resolved home channel, or Watch cannot hear the reference train at all and
both arms measure nothing. The script checks the frequency and refuses
otherwise. Requires explicit --allow-transmit; the transmitter is quieted on
every exit path.
"""

import argparse
import math
import pathlib
import statistics
import sys
import threading
import time

import serial

from bench_harness import (CARD_MARKER, TX_MARKER, Endpoint, ResultWriter,
                           card_status, parse_fields, require_ack)


# bench/heltec-v4r8-transmitter/src/main.cpp: 918.5 MHz, SF8, BW125, CR4/5,
# sync 0x2B — the one candidate that matches this bench's resolved home
# channel, which is what makes Watch able to receive the reference train.
HOME_CANDIDATE = "MESH_OREGON"
HOME_CANDIDATE_MHZ = 918.5


def wilson_interval(successes, total, z=1.959963984540054):
    if total == 0:
        return (0.0, 1.0)
    p = successes / total
    denom = 1.0 + z * z / total
    center = (p + z * z / (2 * total)) / denom
    margin = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denom
    return (max(0.0, center - margin), min(1.0, center + margin))


class PulseTrain:
    """Fires the reference train on its own schedule, independent of Focus.

    The transmitter is the timing source (§6.1): if the pulse cadence were
    driven by the receiver's request loop, an arm that spent longer away would
    also be sent fewer packets, and the comparison would measure the harness.
    """

    def __init__(self, transmitter, interval_s):
        self.transmitter = transmitter
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thread = None
        self.sent = 0
        self.failed = 0

    def _run(self):
        next_fire = time.monotonic()
        while not self._stop.is_set():
            now = time.monotonic()
            if now < next_fire:
                self._stop.wait(min(0.05, next_fire - now))
                continue
            next_fire += self.interval_s
            try:
                require_ack(self.transmitter, "ARM", "0", timeout=1.5)
                self.sent += 1
            except (RuntimeError, TimeoutError, OSError):
                # One dropped ARM must not end the train; it is counted so the
                # reference total stays honest.
                self.failed += 1

    def __enter__(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        return False


def run_arm(card, results, label, duration_s, train, focus_request):
    """One timed arm. `focus_request` is None for the Watch-only baseline."""
    before = card_status(card)
    start_rxp = int(before.get("RXP", "0"))
    start_sent = train.sent
    started = time.monotonic()

    focus_completed = 0
    focus_refused = 0
    away_ms_total = 0
    watch_gaps_s = []
    last_restore = started

    while time.monotonic() - started < duration_s:
        if focus_request is None:
            time.sleep(0.25)
            continue
        bin_index, dwell_ms, samples = focus_request
        opcode, payload = card.request("BENCH_FOCUS", f"{bin_index}:{dwell_ms}:{samples}",
                                       timeout=5.0)
        if opcode != "ACK":
            focus_refused += 1
            continue
        deadline = time.monotonic() + dwell_ms / 1000.0 + 10.0
        while time.monotonic() < deadline:
            status = card_status(card)
            if status.get("FS") == "3":
                break
        else:
            raise RuntimeError("a Focus request did not reach a terminal state in this arm")
        result = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "HEALTH"))
        if result.get("HR") != "1":
            raise RuntimeError(f"Focus failed to restore home listening: {result}")
        focus_completed += 1
        away_ms_total += int(card_status(card).get("FA", "0"))
        now = time.monotonic()
        watch_gaps_s.append(now - last_restore)
        last_restore = now

    elapsed = time.monotonic() - started
    after = card_status(card)
    received = int(after.get("RXP", "0")) - start_rxp
    reference = train.sent - start_sent
    low, high = wilson_interval(min(received, reference), reference)

    row = {
        "event": "arm",
        "arm": label,
        "elapsed_s": round(elapsed, 2),
        "reference_pulses": reference,
        "received_packets": received,
        "received_fraction": round(received / reference, 4) if reference else None,
        "wilson95": [round(low, 4), round(high, 4)],
        "focus_completed": focus_completed,
        "focus_refused": focus_refused,
        "focus_away_ms_total": away_ms_total,
        "away_fraction": round(away_ms_total / (elapsed * 1000.0), 4) if elapsed else None,
        "watch_gap_s_median": round(statistics.median(watch_gaps_s), 2) if watch_gaps_s else None,
        "watch_gap_s_max": round(max(watch_gaps_s), 2) if watch_gaps_s else None,
        "crc_errors": int(after.get("RXC", "0")) - int(before.get("RXC", "0")),
        "train_failed_arms": train.failed,
    }
    results.write(row)
    return row


def describe(row):
    fraction = "n/a" if row["received_fraction"] is None else f"{row['received_fraction']:.3f}"
    print(f"  {row['arm']:<14s} received {row['received_packets']}/{row['reference_pulses']} "
          f"= {fraction}  95% CI [{row['wilson95'][0]:.3f}, {row['wilson95'][1]:.3f}]")
    if row["focus_completed"]:
        print(f"                 Focus {row['focus_completed']} completed, "
              f"{row['focus_refused']} refused, away {row['focus_away_ms_total']} ms "
              f"({row['away_fraction']:.1%} of the arm)")
        print(f"                 restored-Watch gaps: median {row['watch_gap_s_median']}s, "
              f"max {row['watch_gap_s_max']}s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--duration-s", type=int, default=180, help="per arm")
    parser.add_argument("--pulse-interval-s", type=float, default=2.0)
    parser.add_argument("--bin", type=int, default=43, help="Focus bin for the interleaved arm")
    parser.add_argument("--dwell-ms", type=int, default=2000)
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--allow-transmit", action="store_true")
    args = parser.parse_args()

    if not args.allow_transmit:
        parser.error("this comparison arms the transmitter; pass --allow-transmit")
    if args.pulse_interval_s < 0.5:
        parser.error("--pulse-interval-s below 0.5 risks overlapping airtime at SF8")
    if not 2 <= args.dwell_ms <= 2000:
        parser.error("--dwell-ms must be 2..2000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results)

    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            status = card_status(card)
            home_khz = int(status.get("F", "0"))
            if abs(home_khz - HOME_CANDIDATE_MHZ * 1000) > 1:
                raise RuntimeError(
                    f"home channel is {home_khz} kHz but the reference train is "
                    f"{HOME_CANDIDATE} at {HOME_CANDIDATE_MHZ} MHz; Watch could not hear it "
                    "and both arms would measure nothing")
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "QUIET", "-")
            require_ack(transmitter, "CONFIG", HOME_CANDIDATE)
            results.write({"event": "boot", "identity": identity, "status": status,
                           "duration_s": args.duration_s,
                           "pulse_interval_s": args.pulse_interval_s,
                           "focus": {"bin": args.bin, "dwell_ms": args.dwell_ms,
                                     "samples": args.samples}})

            print(f"reference train: {HOME_CANDIDATE} every {args.pulse_interval_s}s, "
                  f"{args.duration_s}s per arm")
            with PulseTrain(transmitter, args.pulse_interval_s) as train:
                baseline = run_arm(card, results, "watch-only", args.duration_s, train, None)
                describe(baseline)
                focus = run_arm(card, results, "watch+focus", args.duration_s, train,
                                (args.bin, args.dwell_ms, args.samples))
                describe(focus)

            if baseline["received_fraction"] is not None and focus["received_fraction"] is not None:
                delta = focus["received_fraction"] - baseline["received_fraction"]
                print(f"\ndelta: {delta:+.3f} received fraction with Focus interleaved")
                print("Read this against away_fraction, not alone: a small loss at a small")
                print("away fraction says nothing about a larger one. Overlapping intervals")
                print("mean the arms are not distinguishable at this trial count.")
            print("\nThis is the measurement §6.3 asks for. Accepting a radio-away budget "
                  "is a separate operator decision.")
        finally:
            if transmitter is not None:
                try:
                    require_ack(transmitter, "QUIET", "-", timeout=5.0)
                finally:
                    transmitter.close()
            card.close()
            results.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("interrupted; completed arms are already durable", file=sys.stderr)
        sys.exit(130)
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase12 Watch opportunity: {error}", file=sys.stderr)
        sys.exit(2)
