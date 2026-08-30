#!/usr/bin/env python3
"""Guided raw-keycode capture for the Cardputer-ADV keyboard.

Every key constant in src/keyboard.h was derived on paper from Launcher's
mapRawKeyToPhysical() and RetroBreeze's _key_value_map, and the host tests
only ever pinned each constant against itself -- which is how a Ctrl+S
modifier chord for repeat-Sweep shipped green three times before this
script's KEY_DUMP capture (2026-08-30) caught it dropping Ctrl's own
release event on real hardware, leaving repeat mode stuck on until reboot.
That chord was reverted in favor of a dedicated key (R); see keyboard.h's
top-of-file note. This script closes the same loop for every other key: it
enables the KEY_DUMP diagnostic, prompts for one key at a time, and reports
what the TCA8418 actually emitted against what keyboard.h claims.

Serial Control must already be enabled on the device (System menu). KEY_DUMP
is a production opcode, not a bench-only one -- it reads nothing and owns no
subsystem, so this runs against the shipping image.

Transport, framing and retries live in bench_harness; this module is capture
policy only.
"""

import argparse
import pathlib
import re
import sys
import time

from bench_harness import CARD_MARKER, Endpoint, ResultWriter, require_ack

# (label, expected raw press byte, expected row, expected col) -- the claims
# keyboard.h makes, in the order the operator is asked to press them.
EXPECTED = [
    ("S", 17, 2, 3),
    ("R", 22, 1, 4),
    ("P", 52, 1, 10),
    ("Enter", 67, 2, 13),
    ("Backtick/ESC", 1, 0, 0),
    ("Comma", 54, 3, 10),
    ("Period", 58, 3, 11),
    ("Semicolon", 57, 2, 11),
    ("Slash", 64, 3, 12),
]

DUMP = re.compile(
    r"\[keydump\] raw=0x(?P<raw>[0-9A-Fa-f]{2}) K=(?P<k>\d+) (?P<edge>DN|UP) "
    r"row=(?P<row>[\d?]+) col=(?P<col>[\d?]+)"
)


def drain(card: Endpoint, seconds: float):
    """Collect [keydump] events for a fixed window."""
    events = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for raw in card._poll_lines():
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            card.record("< " + text)
            match = DUMP.search(text)
            if match:
                events.append(match.groupdict())
        time.sleep(0.005)
    return events


def capture_single(card, writer, label, expect_raw, expect_row, expect_col, window):
    input(f"\n>>> Press and release [{label}], then hit Enter here... ")
    events = drain(card, window)
    presses = [e for e in events if e["edge"] == "DN"]
    result = {
        "step": "single",
        "label": label,
        "expected_raw": expect_raw,
        "expected_row": expect_row,
        "expected_col": expect_col,
        "events": events,
    }
    if not presses:
        result["verdict"] = "NO_EVENT"
        print(f"    FAIL  {label}: no press event seen at all")
    else:
        got = int(presses[0]["k"])
        result["observed_raw"] = got
        result["observed_row"] = presses[0]["row"]
        result["observed_col"] = presses[0]["col"]
        if got == expect_raw:
            result["verdict"] = "MATCH"
            print(f"    ok    {label}: K={got} (row {presses[0]['row']}, col {presses[0]['col']})")
        else:
            result["verdict"] = "MISMATCH"
            print(
                f"    FAIL  {label}: keyboard.h says K={expect_raw} "
                f"(row {expect_row}, col {expect_col}), hardware says K={got} "
                f"(row {presses[0]['row']}, col {presses[0]['col']})"
            )
        if len(presses) > 1:
            print(f"    note  {label}: {len(presses)} press events, using the first")
    writer.write(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--card", required=True, help="Cardputer serial port")
    parser.add_argument("--results", default="keymap_capture.jsonl")
    parser.add_argument("--log", default="keymap_capture.log")
    parser.add_argument(
        "--window",
        type=float,
        default=0.4,
        help="seconds to drain after each prompt (default 0.4)",
    )
    args = parser.parse_args()

    log = pathlib.Path(args.log).open("a", encoding="utf-8")
    writer = ResultWriter(args.results)
    card = Endpoint("cardputer", args.card, CARD_MARKER, log)
    results = []
    try:
        require_ack(card, "HELLO", "-", timeout=20.0)
        require_ack(card, "KEY_DUMP", "ON")
        print("\nKEY_DUMP armed. Press exactly the key named, nothing else.")
        for label, raw, row, col in EXPECTED:
            results.append(capture_single(card, writer, label, raw, row, col, args.window))
    finally:
        try:
            require_ack(card, "KEY_DUMP", "OFF")
        except Exception as exc:  # noqa: BLE001 - best-effort disarm
            print(f"warning: could not disarm KEY_DUMP: {exc}", file=sys.stderr)
        writer.close()
        card.close()
        log.close()

    bad = [r for r in results if r.get("verdict") != "MATCH"]
    print(f"\n{len(results) - len(bad)}/{len(results)} checks passed.")
    for item in bad:
        print(f"  FAILED: {item.get('label', item['step'])} -> {item.get('verdict')}")
    print(f"Raw events recorded to {args.results}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
