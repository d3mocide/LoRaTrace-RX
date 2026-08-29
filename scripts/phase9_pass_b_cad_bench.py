#!/usr/bin/env python3
"""Measure Pass B CAD_FREE/CAD_DETECTED rate per SF/BW combo, quiet vs pulse.

Requires the dedicated ``cardputer-adv-bench`` image: its bounded
``BENCH_PASS_B_CAD`` selector runs one Pass B CAD attempt at a fixed
MESH_OREGON test frequency for an operator-chosen ``PASS_B_SF_BW_CANDIDATES``
row, independent of any real Pass-A peak. Production Pass B only ever runs
at a bin Pass A has already flagged as an energy peak, so a genuinely quiet
condition never exercises it at all through a real ENERGY_SWEEP -- this is
the only way to get a quiet-condition false-positive baseline per combo.

Each attempt logs through the exact same ``energy.csv`` path production
Pass B uses (``EnergyObservation``, CAD_FREE/CAD_DETECTED/CAD_TIMEOUT).
This script does not read that CSV itself (Serial Control has no
file-system access by design, research/phase8-low-profile-harness-design.md)
-- pull it from the SD card afterward for the actual per-combo rate. This
script only verifies each cycle completed cleanly (bounded, home/SD
retained) and produces a durable event log to correlate against
``energy.csv`` afterward, the same "the harness never treats a serial line
as the definitive assertion" convention every other Phase 8/9 bench here
already follows.
"""

import argparse
import pathlib
import sys

import serial

from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack, wait_for


# Order and names match src/pass_b_plan.h's PASS_B_SF_BW_CANDIDATES exactly.
COMBOS = (
    (0, "SF7_BW62.5"),
    (1, "SF7_BW250"),
    (2, "SF8_BW125"),
    (3, "SF8_BW250"),
    (4, "SF9_BW250"),
    (5, "SF10_BW250"),
    (6, "SF11_BW125"),
    (7, "SF11_BW250"),
    (8, "SF11_BW500"),
    (9, "SF12_BW125"),
)


def run_cycle(card, transmitter, mode, index, name, cycle):
    before = card_status(card)
    if before.get("BPC") == "1":
        raise RuntimeError("Cardputer bench Pass-B CAD trigger is already active")
    if before.get("SD") != "1":
        raise RuntimeError(f"Cardputer is not SD-ready: {before}")
    home_frequency = before.get("F")
    pba_before = int(before.get("PBA", "0"))

    if mode == "pulse":
        # Arm before queueing the Cardputer trigger, with a 0ms delay --
        # same convention phase8_cad_bench.py's own run_cycle already
        # established: native USB delivery is too variable to target a
        # combo after the fact, so firing the pulse immediately on ARM's
        # own ack gives it a natural, deterministic-enough overlap with the
        # CAD window that follows.
        require_ack(transmitter, "ARM", "0")

    # Longer than require_ack's 3.0s default: a 400-cycle unattended run
    # only needs to ride out an occasional native-USB response drop (this
    # request is idempotent to retry -- BENCH_PASS_B_CAD is rejected with
    # BAD_ARGUMENT/UNSUPPORTED if busy, never double-armed), not treat one
    # as a hard failure.
    require_ack(card, "BENCH_PASS_B_CAD", str(index), timeout=6.0)

    terminal = wait_for(
        card,
        lambda status: status.get("BPC") == "0" and int(status.get("PBA", "0")) > pba_before,
        10.0,
        f"bench Pass-B CAD trigger completion (combo {name}, cycle {cycle})",
    )
    if terminal.get("F") != home_frequency:
        raise RuntimeError(f"cycle {cycle}: home tuple was not restored: {terminal}")
    if terminal.get("SD") != "1":
        raise RuntimeError(f"cycle {cycle}: SD did not remain ready: {terminal}")
    if int(terminal.get("PBA", "0")) != pba_before + 1:
        raise RuntimeError(f"cycle {cycle}: PBA advanced by more than one attempt: {terminal}")
    return terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--quiet-cycles", type=int, default=20)
    parser.add_argument("--pulse-cycles", type=int, default=20)
    args = parser.parse_args()
    if not 0 <= args.quiet_cycles <= 100 or not 0 <= args.pulse_cycles <= 100:
        parser.error("cycle counts must be 0..100")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log:
        results = ResultWriter(args.results)
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            if "BENCH=1" not in identity:
                raise RuntimeError("Pass B CAD bench requires cardputer-adv-bench firmware")
            card.record("BOOT_CONFIRMED " + identity)
            results.write({"event": "boot", "identity": identity,
                           "quiet_cycles": args.quiet_cycles, "pulse_cycles": args.pulse_cycles})

            transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", "MESH_OREGON")
            require_ack(transmitter, "QUIET", "-")

            for index, name in COMBOS:
                quiet_ok = 0
                pulse_ok = 0
                for cycle in range(1, args.quiet_cycles + 1):
                    result = run_cycle(card, transmitter, "quiet", index, name, cycle)
                    result.update({"event": "cycle", "mode": "quiet", "combo_index": index,
                                   "combo_name": name, "cycle": cycle})
                    results.write(result)
                    quiet_ok += 1
                for cycle in range(1, args.pulse_cycles + 1):
                    result = run_cycle(card, transmitter, "pulse", index, name, cycle)
                    result.update({"event": "cycle", "mode": "pulse", "combo_index": index,
                                   "combo_name": name, "cycle": cycle})
                    results.write(result)
                    pulse_ok += 1
                summary = {"event": "combo_complete", "combo_index": index, "combo_name": name,
                           "quiet_cycles": quiet_ok, "pulse_cycles": pulse_ok}
                results.write(summary)
                print(summary)

            results.write({"event": "complete", "combos": [name for _, name in COMBOS]})
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
        print(f"phase9 Pass B CAD bench: {error}", file=sys.stderr)
        sys.exit(2)
