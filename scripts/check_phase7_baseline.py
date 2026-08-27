#!/usr/bin/env python3
"""Check the objective A/B gates in a Phase 7 session.csv capture.

The display interaction and no-tearing checks remain deliberately manual;
this tool prevents the numeric receive/logging evidence from being accepted
by inspection alone.
"""

import argparse
import csv
import sys


REQUIRED = {
    "reason",
    "uptime_s",
    "rx",
    "rows",
    "flushes",
    "queue_drop",
    "bus_miss",
    "row_drop",
    "radio_stack_free",
    "gps_stack_free",
    "logger_stack_free",
    "ui_stack_free",
    "wifi_stack_free",
}


def number(row, name):
    value = row.get(name, "")
    try:
        return int(value)
    except (TypeError, ValueError):
        raise ValueError("row %s is not an integer: %r" % (name, value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True, help="session.csv copied from one run")
    parser.add_argument(
        "--ui-ok",
        action="store_true",
        help="confirm the manual no-tearing/menu interaction pass",
    )
    args = parser.parse_args()

    with open(args.session, newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        fields = set(reader.fieldnames or ())
        missing = sorted(REQUIRED - fields)
        if missing:
            print("FAIL: missing session.csv columns: " + ", ".join(missing))
            return 1
        rows = list(reader)

    if not rows:
        print("FAIL: session.csv contains no data rows")
        return 1

    a_failures = []
    b_failures = []
    parsed = []
    for index, row in enumerate(rows, start=2):
        try:
            parsed.append({name: number(row, name) for name in REQUIRED if name != "reason"})
        except ValueError as exc:
            a_failures.append("line %d: %s" % (index, exc))

    if a_failures:
        for failure in a_failures:
            print("FAIL: " + failure)
        return 1

    boot_rows = [row for row in rows if row.get("reason") == "boot"]
    if not boot_rows:
        a_failures.append("no reason=boot row")

    max_uptime = max(row["uptime_s"] for row in parsed)
    if max_uptime < 600:
        a_failures.append("only %ds recorded; Baseline A needs at least 600s" % max_uptime)

    post_boot = [row for row in parsed if row["uptime_s"] > 0]
    if not post_boot:
        a_failures.append("no post-boot telemetry rows")
    else:
        for task in ("radio", "gps", "logger", "ui", "wifi"):
            field = task + "_stack_free"
            if not any(row[field] > 0 for row in post_boot):
                a_failures.append("%s stack watermark was never recorded" % task)

    max_rx = max(row["rx"] for row in parsed)
    max_rows = max(row["rows"] for row in parsed)
    max_flushes = max(row["flushes"] for row in parsed)
    if max_rx < 100 or max_rows < 100:
        b_failures.append("receive/log totals are rx=%d rows=%d; Baseline B needs 100/100" %
                          (max_rx, max_rows))
    if max_flushes < 2:
        b_failures.append("only %d detection flushes; Baseline B needs several" % max_flushes)

    for field in ("queue_drop", "bus_miss", "row_drop"):
        maximum = max(row[field] for row in parsed)
        if maximum != 0:
            b_failures.append("%s reached %d" % (field, maximum))

    print("run: rows=%d uptime=%ds rx=%d logged=%d flushes=%d" %
          (len(rows), max_uptime, max_rx, max_rows, max_flushes))
    print("A numeric gate: %s" % ("PASS" if not a_failures else "FAIL"))
    print("B numeric gate: %s" % ("PASS" if not b_failures else "FAIL"))
    print("UI/manual gate: %s" % ("PASS" if args.ui_ok else "PENDING (--ui-ok required)"))
    if not args.ui_ok:
        b_failures.append("manual UI/no-tearing pass was not confirmed")
    failures = a_failures + b_failures
    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
