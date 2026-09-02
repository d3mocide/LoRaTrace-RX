#!/usr/bin/env python3
"""Phase 9 Sweep calibration: known signals at low/mid/high bins.

research/LoRaTrace-Phases-7-10-Design.md's own hardware matrix (11.2)
specifies this as its own test, separate from the 24-hour Endurance row:
"known signals at low/mid/high bins", 30 sweeps. ROADMAP.md's Phase 9 exit
criteria compressed this into "injected low/mid/high carriers land in the
correct bins" -- this script is that test.

A full Sweep only dwells on any one bin for ~tens of ms (docs/STATUS.md's
own rolloff-investigation writeup already hit this once), so a single
beacon pulse or a periodic 2s-interval one has low odds of landing inside
one specific bin's dwell window during one sweep. This fires ARM
repeatedly (0ms delay, ~150ms apart) for the sweep's *entire* duration
instead, giving on the order of 15-20 chances per lap -- reusing the
"repeated attempts across the live window" approach BENCH_RSSI_WINDOW's
own bench script already established, adapted from a fixed ~2s window to
a real Sweep's own several-second one.

Requires the cardputer-adv-bench image (BENCH_SWEEP_FLOOR is bench-only).
"""

import argparse
import pathlib
import sys
import threading
import time
from datetime import datetime, timezone

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack

# Sourced from bench/heltec-v4r8-transmitter/src/main.cpp's own CANDIDATES
# table, spread across the US 902-923MHz region (region_plan.h) -- not
# inventing new frequencies.
LOW_MID_HIGH = {
    'low': ('LONG_SLOW', 905.3125),
    'mid': ('LONG_MODERATE', 912.8125),
    'high': ('SHORT_SLOW', 920.625),
}

BAND_HI_MHZ = 923.0  # fixed hardware ceiling regardless of region (energy_plan.h)
BIN_STEP_MHZ = 0.25
SWEEP_TERMINAL_TIMEOUT_S = 60.0
STATUS_POLL_INTERVAL_S = 0.15
FIRE_INTERVAL_S = 0.1

# Ambient floor across this bench's own sessions has consistently read
# -103 to -113dBm (docs/STATUS.md's rolloff work, this project's own
# edge-carrier/sync_capture sessions); a real nearby transmission reads
# tens of dB above that. This threshold sits well above realistic ambient
# noise and well below any real captured signal seen so far.
DETECTED_THRESHOLD_DBM = -90.0


def bin_for_freq(freq_mhz: float, total_bins: int) -> int:
    lo_mhz = BAND_HI_MHZ - (total_bins - 1) * BIN_STEP_MHZ
    idx = round((freq_mhz - lo_mhz) / BIN_STEP_MHZ)
    return max(0, min(total_bins - 1, idx))


def fire_loop(transmitter: Endpoint, stop: threading.Event):
    while not stop.is_set():
        try:
            require_ack(transmitter, 'ARM', '0', timeout=1.0)
        except (RuntimeError, TimeoutError):
            pass
        stop.wait(FIRE_INTERVAL_S)


def run_sweep_bombarded(card: Endpoint, transmitter: Endpoint):
    before = card_status(card)
    if before.get('W') == 'RUNNING':
        raise RuntimeError(f'Sweep already active before start: {before}')
    home_frequency = before.get('F')

    require_ack(card, 'SWEEP_START', '-')
    stop = threading.Event()
    fireter = threading.Thread(target=fire_loop, args=(transmitter, stop), daemon=True)
    fireter.start()

    deadline = time.monotonic() + SWEEP_TERMINAL_TIMEOUT_S
    status = None
    try:
        while time.monotonic() < deadline:
            status = card_status(card)
            if status.get('W') in {'COMPLETE', 'CANCELLED', 'FAILED'}:
                break
            time.sleep(STATUS_POLL_INTERVAL_S)
        else:
            raise TimeoutError(f'Sweep did not reach a terminal state within {SWEEP_TERMINAL_TIMEOUT_S}s')
    finally:
        stop.set()
        fireter.join(timeout=2.0)

    if status.get('W') != 'COMPLETE':
        raise RuntimeError(f'Sweep ended {status.get("W")}, not COMPLETE: {status}')
    if status.get('F') != home_frequency:
        raise RuntimeError(f'Home frequency not restored: {status}')
    return status


def query_floor(card: Endpoint, bin_index: int):
    opcode, payload = card.request('BENCH_SWEEP_FLOOR', str(bin_index), timeout=6.0)
    if opcode != 'ACK':
        return None
    fields = dict(item.split('=', 1) for item in payload.split(';') if '=' in item)
    return int(fields['FLOOR']) / 10.0 if 'FLOOR' in fields else None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cardputer-port', required=True)
    parser.add_argument('--heltec-port', required=True)
    parser.add_argument('--repeats', type=int, default=5,
                         help='sweeps per low/mid/high candidate (research doc specifies 30 '
                              'total for this test; default is a quicker first pass)')
    parser.add_argument('--log', required=True)
    parser.add_argument('--results', default=None)
    args = parser.parse_args()

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    results = ResultWriter(args.results) if args.results else None

    with log_path.open('a', encoding='utf-8') as log:
        card = Endpoint('cardputer', args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            boot_identity = require_ack(card, 'HELLO', '-', timeout=15.0)
            card.record('BOOT_CONFIRMED ' + boot_identity)
            if 'BENCH=1' not in boot_identity:
                raise RuntimeError(
                    f'Cardputer is not running the bench image (BENCH_SWEEP_FLOOR needs it): {boot_identity}')
            if results:
                results.write({'event': 'boot', 'identity': boot_identity})

            transmitter = Endpoint('heltec', args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, 'HELLO', '-')
            require_ack(transmitter, 'QUIET', '-')

            summary = []
            for label, (candidate, freq_mhz) in LOW_MID_HIGH.items():
                require_ack(transmitter, 'CONFIG', candidate)
                print(f'\n{label}: {candidate} ({freq_mhz}MHz), {args.repeats} sweeps')

                hits = 0
                attempted = 0
                for i in range(args.repeats):
                    # A dropped response over native USB (this project's own
                    # documented, already-known transport characteristic --
                    # see docs/research/phase9-sweep-pass-b-design.md) costs
                    # one lap here, not the whole run: log and move on.
                    try:
                        status = run_sweep_bombarded(card, transmitter)
                    except (RuntimeError, TimeoutError) as error:
                        print(f'  [{i}] SKIPPED: {error}')
                        if results:
                            results.write({'event': 'attempt_skipped', 'label': label,
                                            'attempt': i, 'error': str(error)})
                        continue
                    attempted += 1
                    total_bins = int(status['WN'])
                    target_bin = bin_for_freq(freq_mhz, total_bins)
                    floor = query_floor(card, target_bin)
                    hit = floor is not None and floor >= DETECTED_THRESHOLD_DBM
                    hits += 1 if hit else 0
                    print(f'  [{i}] bin={target_bin}/{total_bins} floor={floor}dBm '
                          f'{"HIT" if hit else "miss"}')
                    if results:
                        results.write({
                            'event': 'attempt', 'label': label, 'candidate': candidate,
                            'freq_mhz': freq_mhz, 'attempt': i, 'target_bin': target_bin,
                            'total_bins': total_bins, 'floor_dbm': floor, 'hit': hit,
                        })

                print(f'  {hits}/{attempted} landed in the correct bin '
                      f'({args.repeats - attempted} skipped)')
                summary.append((label, candidate, freq_mhz, hits, attempted))
                if results:
                    results.write({'event': 'candidate_summary', 'label': label,
                                    'hits': hits, 'total': attempted})

            print(f'\n{"band":<6} {"candidate":<16} {"freq":>10} {"hits":>8}')
            for label, candidate, freq_mhz, hits, total in summary:
                print(f'{label:<6} {candidate:<16} {freq_mhz:>8.4f}M {hits:>5}/{total}')
        finally:
            if transmitter is not None:
                require_ack(transmitter, 'QUIET', '-')
                transmitter.close()
            card.close()
            if results:
                results.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
