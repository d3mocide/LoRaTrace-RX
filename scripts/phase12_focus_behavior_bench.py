#!/usr/bin/env python3
"""Non-transmitting Phase 12 Focus cancel/failure/timeout/arbitration checks.

Uses only framed Serial Control and the bench image. It records Focus's
durable row after cancellation, a one-shot injected failure, and an injected
sample-loop stall past the request deadline, then verifies mutual refusal with
Probe, Sweep, Cell, and Scope in both directions. Nothing here transmits, and
none of it sets a coverage or activity threshold.
"""

import argparse
import pathlib
import sys
import time

import serial

from bench_harness import (CARD_MARKER, Endpoint, ResultWriter, card_status,
                           parse_fields, require_ack, wait_for)


FOCUS_ARGUMENT = "43:2000:64"  # US 912.750 MHz bin; long enough to arbitrate.
# 100ms dwell -> a 1,100ms deadline (focus_plan.h), so a 1,500ms bench stall
# overruns it deterministically instead of waiting on real bus contention.
FOCUS_TIMEOUT_ARGUMENT = "43:100:4:1500"
FOCUS_TIMEOUT_STALL_MS = 1500
# Stall, deadline, and one restore: past this the request was not bounded.
FOCUS_TIMEOUT_AWAY_LIMIT_MS = 4000


def expect_error(card, command, argument, payload):
    opcode, actual = card.request(command, argument, timeout=5.0)
    if opcode != "ERROR" or actual != payload:
        raise RuntimeError(f"{command} expected ERROR {payload}, got {opcode} {actual}")


def start_focus(card):
    card.send("BENCH_FOCUS", FOCUS_ARGUMENT)
    # The framed request is already on USB; allow Core 1 to claim its mailbox
    # before sending the conflicting command, without relying on debug text.
    time.sleep(0.35)


def cancel_focus(card, prior_written):
    card.send("BENCH_FOCUS_CANCEL", "-")
    terminal = wait_for(
        card,
        lambda status: status.get("FS") == "4"
        and int(status.get("FW", "0")) > prior_written,
        10.0,
        "cancelled Focus durable row",
    )
    result = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "HEALTH"))
    if result.get("RS") != "1" or result.get("HR") != "1":
        raise RuntimeError(f"cancelled Focus did not restore: {result}")
    return terminal, result


def action_state(card):
    return parse_fields(require_ack(card, "BENCH_ACTION", "STATE"))


def wait_for_action_idle(card, key, timeout, description):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = action_state(card)
        if last.get(key) == "0":
            return last
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for {description}; last state={last}")


def check_refuses_focus(card, action, key, results, event):
    """Prove the named bounded action refuses a Focus request while it owns RX."""
    require_ack(card, "BENCH_ACTION", f"{action}:START")
    time.sleep(0.35)
    busy = action_state(card)
    if busy.get(key) != "1":
        raise RuntimeError(f"{action} did not take the radio: {busy}")
    expect_error(card, "BENCH_FOCUS", "43:500:8", "UNAVAILABLE")
    require_ack(card, "BENCH_ACTION", f"{action}:CANCEL")
    terminal = wait_for_action_idle(card, key, 20.0, f"{action} to release the radio")
    record(results, event, busy=busy, terminal=terminal)


def check_focus_refuses(card, action, key, results, event):
    """Prove Focus refuses the named bounded action while Focus owns RX."""
    before = card_status(card)
    start_focus(card)
    expect_error(card, "BENCH_ACTION", f"{action}:START", "UNAVAILABLE")
    during = action_state(card)
    if during.get(key) != "0":
        raise RuntimeError(f"{action} started while Focus owned RX: {during}")
    terminal, result = cancel_focus(card, int(before["FW"]))
    record(results, event, during=during, status=terminal, result=result)


def record(writer, event, **fields):
    if writer is not None:
        writer.write({"event": event, **fields})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw framed-control log")
    parser.add_argument("--results", help="append-only JSONL summary path")
    args = parser.parse_args()

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        try:
            identity = require_ack(card, "HELLO", "-", timeout=15.0)
            initial = card_status(card)
            if initial.get("SD") != "1":
                raise RuntimeError(f"Focus requires an SD-backed result row: {initial}")
            record(results, "boot", identity=identity, status=initial)

            start_focus(card)
            card.send("PROBE_START", "-")
            time.sleep(0.05)
            cancel_status, cancel_result = cancel_focus(card, int(initial["FW"]))
            # Native USB can fragment a one-way ERROR frame. The framed
            # terminal STATUS is stronger evidence: a request accepted while
            # Focus owned RX would have entered/raced Probe after cancel.
            if cancel_status.get("B") != "IDLE":
                raise RuntimeError(f"Probe started while Focus owned RX: {cancel_status}")
            record(results, "focus_refuses_probe", status=cancel_status, result=cancel_result)

            before_sweep = card_status(card)
            start_focus(card)
            card.send("SWEEP_START", "-")
            time.sleep(0.05)
            cancel_status, cancel_result = cancel_focus(card, int(before_sweep["FW"]))
            if cancel_status.get("W") != "IDLE":
                raise RuntimeError(f"Sweep started while Focus owned RX: {cancel_status}")
            record(results, "focus_refuses_sweep", status=cancel_status, result=cancel_result)

            require_ack(card, "BENCH_FAULT", "AFTER_RETUNE:FAIL")
            before_failure = card_status(card)
            card.send("BENCH_FOCUS", "43:500:8")
            failed = wait_for(
                card,
                lambda status: status.get("FS") == "6"
                and int(status.get("FW", "0")) > int(before_failure["FW"]),
                10.0,
                "failed Focus durable row",
            )
            failure_result = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "HEALTH"))
            if failure_result.get("RS") != "3" or failure_result.get("HR") != "1" or failure_result.get("E") == "0":
                raise RuntimeError(f"failure did not retain operation error plus restore: {failure_result}")
            record(results, "focus_failure_restores", status=failed, result=failure_result)
            require_ack(card, "BENCH_FAULT", "CLEAR")

            card.send("PROBE_START", "-")
            time.sleep(0.35)
            expect_error(card, "BENCH_FOCUS", "43:500:8", "UNAVAILABLE")
            card.send("PROBE_CANCEL", "-")
            probe_terminal = wait_for(
                card, lambda status: status.get("B") in {"CANCELLED", "COMPLETE", "FAILED"},
                12.0, "Probe terminal",
            )
            record(results, "probe_refuses_focus", status=probe_terminal)

            card.send("SWEEP_START", "-")
            time.sleep(0.35)
            expect_error(card, "BENCH_FOCUS", "43:500:8", "UNAVAILABLE")
            card.send("SWEEP_CANCEL", "-")
            sweep_terminal = wait_for(
                card, lambda status: status.get("W") in {"CANCELLED", "COMPLETE", "FAILED"},
                12.0, "Sweep terminal",
            )
            record(results, "sweep_refuses_focus", status=sweep_terminal)

            # Timeout: a stalled sample loop must stop at its own deadline,
            # restore Watch, and publish `timeout` -- not `complete` borrowed
            # from a restore that worked, and not an unbounded radio-away hold.
            before_timeout = card_status(card)
            card.send("BENCH_FOCUS", FOCUS_TIMEOUT_ARGUMENT)
            timed_out = wait_for(
                card,
                lambda status: status.get("FS") == "5"
                and int(status.get("FW", "0")) > int(before_timeout["FW"]),
                15.0,
                "timed-out Focus durable row",
            )
            timeout_result = parse_fields(require_ack(card, "BENCH_FOCUS_RESULT", "HEALTH"))
            if timeout_result.get("RS") != "2" or timeout_result.get("HR") != "1":
                raise RuntimeError(f"timeout did not restore home: {timeout_result}")
            away_ms = int(timed_out.get("FA", "0"))
            if not FOCUS_TIMEOUT_STALL_MS <= away_ms <= FOCUS_TIMEOUT_AWAY_LIMIT_MS:
                raise RuntimeError(f"timeout radio-away time was not bounded: {away_ms}ms")
            record(results, "focus_timeout_restores", status=timed_out,
                   result=timeout_result, away_ms=away_ms)

            check_focus_refuses(card, "CELL", "CELL", results, "focus_refuses_cell")
            check_refuses_focus(card, "CELL", "CELL", results, "cell_refuses_focus")
            check_focus_refuses(card, "SCOPE", "SCOPE", results, "focus_refuses_scope")
            check_refuses_focus(card, "SCOPE", "SCOPE", results, "scope_refuses_focus")

            print("Focus cancel/failure/timeout and Probe/Sweep/Cell/Scope "
                  "mutual-exclusion checks passed.")
        finally:
            # A failure cannot leave the one-shot injector armed for later work.
            try:
                require_ack(card, "BENCH_FAULT", "CLEAR", timeout=5.0)
            finally:
                card.close()
                if results is not None:
                    results.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase12 Focus behavior bench: {error}", file=sys.stderr)
        sys.exit(2)
