#!/usr/bin/env python3
"""Phase 12 §6.2 controlled dwell matrix: paired source-on/source-off Focus trials.

Collects the evidence the coverage vocabulary needs and nothing more. Each
trial is one independently logged, bounded Focus request — the matrix gets its
30 trials from 30 durable requests, never from one long radio-away loop
(docs/research/phase12-survey-truth-design.md §6.2). It records raw RSSI
summaries per trial and selects no threshold; that is the report script's job,
run offline against this output.

The transmitter is repo-owned (bench/heltec-v4r8-transmitter) and is left quiet
on every exit path. Arming it requires explicit --allow-transmit.

Nothing here decides a coverage label, and an absent observation is recorded as
a measured no-observation at a stated frequency offset — never as "quiet".
"""

import argparse
import pathlib
import random
import sys
import threading
import time

import serial

from bench_harness import (CARD_MARKER, TX_MARKER, Endpoint, ResultWriter,
                           card_status, parse_fields, require_ack, wait_for)


# US Sweep band (energy_plan.h): bin centers are lo + index * step.
US_BAND_LO_MHZ = 902.0
BIN_STEP_MHZ = 0.25

# Sourced from bench/heltec-v4r8-transmitter/src/main.cpp's CANDIDATES table;
# no frequency is invented here. `bin` is the Sweep bin Focus is asked for, and
# `offset_khz` is how far the transmitter actually sits from that bin's center.
#
# That offset is a first-class result field, not a footnote: Focus tunes at the
# resolved home channel's bandwidth (125 kHz on this bench), so a position
# 125 kHz off center is outside the passband half-width and a weak or absent
# rise there measures the offset, not the receiver's sensitivity. The three
# §6.1 positions are kept because they are the sourced low/mid/high set; the
# two `-aligned` positions exist so an arm can be run without that confound.
POSITIONS = {
    # §6.1's sourced low/mid/high set.
    "low": {"candidate": "LONG_SLOW", "tx_mhz": 905.3125, "bin": 13},
    "mid": {"candidate": "LONG_MODERATE", "tx_mhz": 912.8125, "bin": 43},
    "high": {"candidate": "SHORT_SLOW", "tx_mhz": 920.625, "bin": 74},
    # Exactly bin-centered alternatives from the same candidate table.
    "low-aligned": {"candidate": "LONG_TURBO", "tx_mhz": 908.750, "bin": 27},
    "mid-aligned": {"candidate": "MESH_OREGON", "tx_mhz": 918.500, "bin": 66},
}

DEFAULT_DWELLS_MS = (100, 500, 2000)
DEFAULT_POSITIONS = ("low", "mid", "high")


def bin_center_mhz(bin_index):
    return US_BAND_LO_MHZ + bin_index * BIN_STEP_MHZ


def position_offset_khz(position):
    spec = POSITIONS[position]
    return round((spec["tx_mhz"] - bin_center_mhz(spec["bin"])) * 1000.0, 1)


class PulseBurst:
    """Keeps the transmitter firing for as long as a trial's window is open.

    A single armed pulse cannot be scheduled reliably against a short dwell.
    Candidate airtimes across this table span roughly 30 ms (SF8/BW250) to
    ~1 s (SF12/BW125), and the receiver's window opens tens of ms after the
    request is accepted — so one pulse at a fixed delay lands inside a 2,000 ms
    dwell and misses a 100 ms one entirely. That is exactly what the first
    attempt at this matrix measured: at 100 ms, source-on and source-off were
    indistinguishable because the pulse started after the window had closed.

    Firing continuously for the trial's duration makes "source on" mean the
    transmitter was actually radiating while Focus was listening, which is the
    condition §3.1's qualifying-RSSI question needs. It deliberately does NOT
    measure the catch probability of intermittent traffic — §1 already records
    that the dwell-versus-real-traffic comparison was inconclusive, and this
    matrix is not a second attempt at it.
    """

    def __init__(self, transmitter, gap_s):
        self.transmitter = transmitter
        self.gap_s = gap_s
        self._stop = threading.Event()
        self._thread = None
        self.fired = 0
        self.failed = 0

    def _await_tx_done(self, timeout_s=4.0):
        """Block until the in-flight transmission reports TX_DONE.

        Paced by the transmitter's own completion rather than a fixed cadence.
        A fixed interval shorter than the airtime queues overlapping sends —
        measured airtime here ranges from ~10 ms (SF8/BW250) to ~275 ms
        (SF12/BW125) — and the tail then bleeds into the next trial. The first
        burst attempt did exactly that: source-off trials inherited a
        transmission and read -26 dBm, identical to source-on.
        """
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if any(opcode == "TX_DONE" for _, opcode, _ in self.transmitter.observed):
                return True
            # STATUS is idempotent; the round trip is what drains the
            # asynchronous TX_STARTED/TX_DONE frames into `observed`.
            try:
                self.transmitter.request("STATUS", "-", timeout=1.0)
            except (RuntimeError, TimeoutError, OSError):
                pass
        return False

    def _run(self):
        while not self._stop.is_set():
            try:
                self.transmitter.observed.clear()
                require_ack(self.transmitter, "ARM", "0", timeout=1.5)
                self.fired += 1
                self._await_tx_done()
            except (RuntimeError, TimeoutError, OSError):
                self.failed += 1
            self._stop.wait(self.gap_s)

    def __enter__(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=8.0)
        # The trial is over, but a transmission may still be in the air. A
        # source-off trial that inherits it is not a source-off trial.
        self._await_tx_done()
        return False


def counters(status):
    """The health counters a trial must not disturb."""
    return {field: int(status.get(field, "0")) for field in ("FO", "FW", "FD", "FL")}


def run_focus_request(card, spec, dwell_ms, samples, start):
    # The long timeout is transport, not RF: the device's native USB CDC
    # occasionally truncates an outbound frame under sustained round trips, and
    # require_ack re-sends once a second within its own timeout. The command is
    # idempotent on the device (one cached response per sequence), so a re-send
    # cannot start a second survey.
    require_ack(card, "BENCH_FOCUS", f"{spec['bin']}:{dwell_ms}:{samples}", timeout=12.0)
    terminal = wait_for(
        card,
        lambda status: status.get("FS") == "3"
        and int(status.get("FO", "0")) > start["FO"]
        and int(status.get("FW", "0")) > start["FW"],
        max(12.0, dwell_ms / 1000.0 + 10.0),
        "Focus completion, home restore, and focus.csv commit",
    )
    return terminal, parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "-", timeout=12.0))


def run_trial(card, transmitter, position, dwell_ms, samples, source_on, gap_ms):
    """One bounded Focus request, with the transmitter radiating throughout it or not at all."""
    spec = POSITIONS[position]
    before = card_status(card)
    start = counters(before)

    pulses = 0
    if source_on:
        with PulseBurst(transmitter, gap_ms / 1000.0) as burst:
            terminal, result = run_focus_request(card, spec, dwell_ms, samples, start)
        pulses = burst.fired
        if burst.fired == 0:
            raise RuntimeError("the transmitter never fired during a source-on trial")
    else:
        terminal, result = run_focus_request(card, spec, dwell_ms, samples, start)

    # §6.2: an arm is rejected if it loses ownership, cannot restore home, or
    # has unexplained queue/row drops. Fail the trial, not the whole matrix's
    # credibility after the fact.
    if result.get("RS") != "0" or result.get("HR") != "1":
        raise RuntimeError(f"trial did not complete with home restore: {result}")
    if int(result.get("N", "0")) != samples:
        raise RuntimeError(f"trial sample count mismatch: expected {samples}, got {result}")
    end = counters(terminal)
    if end["FD"] != start["FD"] or end["FL"] != start["FL"]:
        raise RuntimeError(f"trial dropped a queued or durable row: {start} -> {end}")

    if source_on:
        transmitter.observed.clear()

    return {
        "position": position,
        "candidate": spec["candidate"],
        "tx_mhz": spec["tx_mhz"],
        "bin": spec["bin"],
        "bin_center_mhz": round(bin_center_mhz(spec["bin"]), 4),
        "offset_khz": position_offset_khz(position),
        "dwell_ms": dwell_ms,
        "samples": samples,
        "source_on": source_on,
        # Tenths of a dBm, exactly as the device reported them.
        "median_dbm_x10": int(result["MED"]),
        "p90_dbm_x10": int(result["P90"]),
        "peak_dbm_x10": int(result["MAX"]),
        "sample_count": int(result["N"]),
        "observation_ms": int(result["OBS"]),
        "focus_id": int(result["ID"]),
        "radio_away_ms": int(terminal.get("FA", "0")),
        "request_status": result["RS"],
        "home_restore": result["HR"],
        "radio_status": int(result["E"]),
        "pulses_fired": pulses,
    }


def settle(card, timeout_s=20.0):
    """Wait until no Focus owns the radio, so a retried trial isn't refused.

    A trial can fail with its request already accepted (the ACK truncated in
    transit). Starting the next one immediately would be refused as
    UNAVAILABLE and look like an arbitration bug rather than a transport one.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            state = card_status(card).get("FS")
        except (RuntimeError, TimeoutError):
            time.sleep(0.5)
            continue
        # 1 = SURVEYING, 2 = RESTORING (focus_runtime.h).
        if state not in {"1", "2"}:
            time.sleep(0.3)
            return True
        time.sleep(0.3)
    return False


def run_trial_with_retry(card, transmitter, position, dwell_ms, samples, source_on,
                         gap_ms, results, attempts):
    """A transport failure is a harness event, not an RF result — retry it, loudly.

    The retry is a genuinely new bounded request, and the failure is recorded
    rather than swallowed: a run whose trials silently retried would overstate
    how cleanly the transport behaved.
    """
    for attempt in range(1, attempts + 1):
        try:
            row = run_trial(card, transmitter, position, dwell_ms, samples,
                            source_on, gap_ms)
            if attempt > 1:
                row["retried_attempts"] = attempt
            return row
        except (RuntimeError, TimeoutError) as error:
            results.write({"event": "trial_error", "position": position,
                           "dwell_ms": dwell_ms, "source_on": source_on,
                           "attempt": attempt, "error": str(error)})
            print(f"    trial attempt {attempt}/{attempts} failed: {error}", flush=True)
            if attempt == attempts:
                raise
            if not settle(card):
                raise RuntimeError("Focus never returned to idle after a failed trial")
            if transmitter is not None:
                try:
                    require_ack(transmitter, "QUIET", "-", timeout=5.0)
                    transmitter.observed.clear()
                except (RuntimeError, TimeoutError):
                    pass


def trial_order(trials, source_order, rng):
    """Alternating by default; --order random shuffles within the arm (§6.2)."""
    plan = [True, False] * trials
    if source_order == "random":
        rng.shuffle(plan)
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw framed-control log")
    parser.add_argument("--results", required=True, help="append-only JSONL trial rows")
    parser.add_argument("--position", action="append", choices=sorted(POSITIONS),
                        help=f"repeatable; default {' '.join(DEFAULT_POSITIONS)}")
    parser.add_argument("--dwell-ms", action="append", type=int,
                        help=f"repeatable; default {' '.join(str(d) for d in DEFAULT_DWELLS_MS)}")
    parser.add_argument("--trials", type=int, default=30,
                        help="trials per source state per arm (§6.2 requires at least 30)")
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--order", choices=("alternating", "random"), default="alternating")
    parser.add_argument("--seed", type=int, default=20260904, help="--order random seed")
    parser.add_argument("--pulse-gap-ms", type=int, default=30,
                        help="idle gap between completed transmissions during a source-on "
                             "trial; the burst is paced by TX_DONE, not by this alone")
    parser.add_argument("--attempts", type=int, default=3,
                        help="attempts per trial before the run gives up (transport retries)")
    parser.add_argument("--allow-transmit", action="store_true",
                        help="required: this matrix arms the controlled transmitter")
    args = parser.parse_args()

    if not args.allow_transmit:
        parser.error("this matrix arms the transmitter; pass --allow-transmit")
    positions = args.position or list(DEFAULT_POSITIONS)
    dwells = args.dwell_ms or list(DEFAULT_DWELLS_MS)
    for dwell in dwells:
        if not 2 <= dwell <= 2000:
            parser.error(f"--dwell-ms {dwell} outside the bounded 2..2000 request range")
    if not 2 <= args.samples <= 64:
        parser.error("--samples must be 2..64")
    if args.trials < 1:
        parser.error("--trials must be positive")
    if args.trials < 30:
        print(f"warning: {args.trials} trials per state is below §6.2's 30; "
              "the result is a smoke check, not matrix evidence", file=sys.stderr)

    rng = random.Random(args.seed)
    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results)
    completed = 0
    started = time.monotonic()

    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            boot = card_status(card)
            if boot.get("SD") != "1":
                raise RuntimeError(f"the matrix needs durable focus.csv rows: {boot}")
            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "QUIET", "-")
            results.write({"event": "boot", "identity": identity, "status": boot,
                           "positions": positions, "dwells_ms": dwells,
                           "trials_per_state": args.trials, "samples": args.samples,
                           "order": args.order, "seed": args.seed})

            for position in positions:
                require_ack(transmitter, "CONFIG", POSITIONS[position]["candidate"])
                for dwell in dwells:
                    plan = trial_order(args.trials, args.order, rng)
                    print(f"[{position} {dwell}ms] {len(plan)} trials "
                          f"(offset {position_offset_khz(position):+.1f} kHz)", flush=True)
                    for index, source_on in enumerate(plan):
                        row = run_trial_with_retry(card, transmitter, position, dwell,
                                                   args.samples, source_on,
                                                   args.pulse_gap_ms, results,
                                                   args.attempts)
                        row.update({"event": "trial", "arm_index": index})
                        results.write(row)
                        completed += 1
                        state = "on " if source_on else "off"
                        print(f"  [{index + 1:3d}/{len(plan)}] source={state} "
                              f"p90={row['p90_dbm_x10'] / 10:.1f}dBm "
                              f"peak={row['peak_dbm_x10'] / 10:.1f}dBm "
                              f"away={row['radio_away_ms']}ms", flush=True)
                        # Let the device's USB CDC buffer drain between trials.
                        time.sleep(0.15)
            elapsed = time.monotonic() - started
            print(f"{completed} trials completed in {elapsed / 60:.1f} min. "
                  "Raw rows only — run phase12_focus_matrix_report.py to analyze.")
        finally:
            # A transmitter left armed after a failure is the one outcome this
            # script must never produce.
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
        print("interrupted; partial rows are already durable", file=sys.stderr)
        sys.exit(130)
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase12 Focus matrix: {error}", file=sys.stderr)
        sys.exit(2)
