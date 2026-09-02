#!/usr/bin/env python3
"""Phase 9 endurance soak: repeated Sweep laps over a long unattended run.

ROADMAP.md's exit criterion: "24 hours of repeated sweeps show bounded
memory and reliable recovery." Scoped to 8 hours by operator decision
(2026-09-02) -- no cited technical derivation exists anywhere in this
project's docs for 24 specifically (docs/STATUS.md/ROADMAP.md), and 8
hours of back-to-back US-region laps (~3.4s device-time each) is still on
the order of several thousand cycles, well past where a real leak or
stability bug would be expected to show up if one exists. Documented here
as a deliberate, reasoned deviation, same convention Phase 7's own
soak/repetition criterion was relaxed under once
(docs/history/PROGRESS.md).

Runs on PRODUCTION firmware, not the bench image -- this validates real
operator behavior (repeated SWEEP_START, home-restore, WiFi lifecycle),
not a bench-only code path. Mirrors
research/LoRaTrace-Phases-7-10-Design.md's own Endurance row ("Off, then
On" WiFi): starts WiFi off, switches it on partway through and leaves it
on for the remainder, so the run covers both conditions over a long
duration rather than just the short matched pairs already checked
(docs/STATUS.md).

"Bounded memory" evidence is NOT this script's job -- session.csv's own
periodic heap/stack health record (logger_task.h, CLAUDE.md's
session_log.h) already samples that independently throughout any run, SD
card permitting. Pull session.csv after this finishes (WIFI_SET ON + the
web UI's Downloads tab, or physically) and inspect the heap_free/
heap_largest trend the way scripts/check_phase7_baseline.py already does
for Phase 7's own soak. This script's own job is "reliable recovery":
every lap completes, home is restored, and the connection stays
responsive, for the whole duration.
"""

import argparse
import pathlib
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from bench_harness import CARD_MARKER, Endpoint, ResultWriter, card_status, require_ack

LAP_TERMINAL_TIMEOUT_S = 90.0
STATUS_POLL_INTERVAL_S = 0.5
PROGRESS_EVERY_N_LAPS = 25


def run_lap(card: Endpoint):
    before = card_status(card)
    if before.get('W') == 'RUNNING':
        raise RuntimeError(f'Sweep already active before start: {before}')
    home_frequency = before.get('F')

    require_ack(card, 'SWEEP_START', '-', timeout=8.0)
    deadline = time.monotonic() + LAP_TERMINAL_TIMEOUT_S
    status = None
    while time.monotonic() < deadline:
        status = card_status(card)
        if status.get('W') in {'COMPLETE', 'CANCELLED', 'FAILED'}:
            break
        time.sleep(STATUS_POLL_INTERVAL_S)
    else:
        raise TimeoutError(f'Sweep did not reach a terminal state within {LAP_TERMINAL_TIMEOUT_S}s')

    if status.get('W') != 'COMPLETE':
        raise RuntimeError(f'Lap ended {status.get("W")}, not COMPLETE: {status}')
    if status.get('F') != home_frequency:
        raise RuntimeError(f'Home frequency not restored: {status}')
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cardputer-port', required=True)
    parser.add_argument('--duration-hours', type=float, default=8.0)
    parser.add_argument('--wifi-on-after-hours', type=float, default=4.0,
                         help='switch WiFi on after this many hours and leave it on for the '
                              'rest of the run (research doc\'s own "Off, then On" Endurance row)')
    parser.add_argument('--log', required=True)
    parser.add_argument('--results', required=True)
    args = parser.parse_args()

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results)

    start = time.monotonic()
    end_time = start + args.duration_hours * 3600.0
    wifi_on_at = start + args.wifi_on_after_hours * 3600.0
    wifi_switched = False

    with log_path.open('a', encoding='utf-8') as log:
        card = Endpoint('cardputer', args.cardputer_port, CARD_MARKER, log)
        try:
            boot_identity = require_ack(card, 'HELLO', '-', timeout=15.0)
            card.record('SOAK_START ' + boot_identity)
            results.write({
                'event': 'start', 'identity': boot_identity,
                'duration_hours': args.duration_hours, 'wifi_on_after_hours': args.wifi_on_after_hours,
                'started_utc': datetime.now(timezone.utc).isoformat(),
            })
            print(f'soak started: {args.duration_hours}h, WiFi on after {args.wifi_on_after_hours}h')

            lap = 0
            failures = 0
            consecutive_failures = 0
            while time.monotonic() < end_time:
                lap += 1

                if not wifi_switched and time.monotonic() >= wifi_on_at:
                    try:
                        require_ack(card, 'WIFI_SET', 'ON', timeout=8.0)
                        wifi_switched = True
                        results.write({'event': 'wifi_on', 'lap': lap,
                                        'elapsed_h': (time.monotonic() - start) / 3600.0})
                        print(f'[lap {lap}] WiFi switched ON for the remainder of the run')
                    except (RuntimeError, TimeoutError) as error:
                        print(f'[lap {lap}] WIFI_SET ON failed, will retry next lap: {error}')

                try:
                    status = run_lap(card)
                    consecutive_failures = 0
                    results.write({
                        'event': 'lap', 'lap': lap, 'elapsed_h': (time.monotonic() - start) / 3600.0,
                        'ea_ms': int(status.get('EA', 0)), 'wp': int(status.get('WP', 0)),
                        'wifi': status.get('WIFI'), 'f': status.get('F'),
                    })
                except (RuntimeError, TimeoutError) as error:
                    failures += 1
                    consecutive_failures += 1
                    results.write({'event': 'lap_failed', 'lap': lap,
                                    'elapsed_h': (time.monotonic() - start) / 3600.0, 'error': str(error)})
                    print(f'[lap {lap}] FAILED: {error} (consecutive={consecutive_failures})')
                    # A single dropped response is this project's own well-documented
                    # native-USB transport characteristic (see docs/research/
                    # phase9-sweep-pass-b-design.md) -- not a device fault. Several in
                    # a row without a single clean lap between them is a different
                    # story: try a plain HELLO on the *same* connection first (no
                    # reset) to see if the device is still there before concluding
                    # anything worse happened.
                    if consecutive_failures >= 3:
                        try:
                            reply = require_ack(card, 'HELLO', '-', timeout=10.0)
                            print(f'[lap {lap}] device still responsive: {reply}')
                            results.write({'event': 'still_alive', 'lap': lap, 'identity': reply})
                        except (RuntimeError, TimeoutError) as hello_error:
                            print(f'[lap {lap}] device NOT responding to HELLO: {hello_error}')
                            results.write({'event': 'unresponsive', 'lap': lap, 'error': str(hello_error)})
                    time.sleep(3.0)

                if lap % PROGRESS_EVERY_N_LAPS == 0:
                    elapsed_h = (time.monotonic() - start) / 3600.0
                    print(f'[lap {lap}] elapsed={elapsed_h:.2f}h failures={failures} '
                          f'({100*failures/lap:.1f}%)')

            elapsed_h = (time.monotonic() - start) / 3600.0
            print(f'\nsoak complete: {lap} laps over {elapsed_h:.2f}h, {failures} failures '
                  f'({100*failures/lap:.1f}%)')
            results.write({'event': 'complete', 'laps': lap, 'failures': failures,
                            'elapsed_h': elapsed_h})
        finally:
            card.close()
            results.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
