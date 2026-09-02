#!/usr/bin/env python3
"""RTL-SDR ground truth for Pass B's standing CAD-at-arbitrary-bin question.

pass_b_plan.h's own comment: "SX1262 CAD-at-arbitrary-bin behavior isn't
verified." The existing bench matrix (scripts/phase9_pass_b_cad_bench.py,
docs/research/phase9-sweep-pass-b-design.md) found CAD_DETECTED's
false-positive rate scales with SF/symbol-duration in a way that looks
like the radio false-triggering on ambient energy at long dwell times, not
a real correlation to any transmitted signal -- but that conclusion was
inferred from aggregate quiet-vs-pulse counts, never directly observed.
This watches the exact frequency (BENCH_PASS_B_CAD's fixed 918.5MHz test
point) with an independent receiver during each CAD attempt, so a quiet-
condition CAD_DETECTED can be checked directly: was anything actually
on the air, or not.

Triggers BENCH_PASS_B_CAD for one PASS_B_SF_BW_CANDIDATES combo per
attempt (radio_task.cpp's performBenchPassBCadTrigger()) and starts a
matching RTL-SDR capture at the same moment, reading back the raw
CAD_FREE/CAD_DETECTED/CAD_TIMEOUT result via BENCH_PASS_B_CAD_RESULT
afterward (bench_fault.h) -- previously only visible by pulling energy.csv
off the SD card. --pulse arms the Heltec during each attempt too, for a
positive control (does CAD reliably fire when something real is
transmitting); the default is quiet-only, matching the false-positive
question this exists to check.

Requires the cardputer-adv-bench image.
"""

import argparse
import pathlib
import sys
import threading
import time
from datetime import datetime, timezone

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'scripts'))
from bench_harness import CARD_MARKER, TX_MARKER, Endpoint, ResultWriter, card_status, require_ack  # noqa: E402

from capture_spectrum import analyze, capture_iq, save_plot  # noqa: E402

# radio_task.cpp's BENCH_PASS_B_CAD_TEST_FREQ_MHZ -- BENCH_PASS_B_CAD always
# runs at this fixed frequency, independent of the combo index (the same
# MESH_OREGON point scripts/phase8_cad_rate_bench.py already uses).
CAD_TEST_FREQ_MHZ = 918.5

# pass_b_plan.h's PASS_B_SF_BW_CANDIDATES, in index order -- kept here
# rather than parsed from the header so this stays a plain Python script;
# cross-check against pass_b_plan.h if that table ever changes.
CANDIDATES = [
    (7, 62.5), (7, 250.0), (8, 125.0), (8, 250.0), (9, 250.0),
    (10, 250.0), (11, 125.0), (11, 250.0), (11, 500.0), (12, 125.0),
]

WINDOW_TERMINAL_TIMEOUT_S = 15.0
STATUS_POLL_INTERVAL_S = 0.1


def pulse_loop(transmitter: Endpoint, stop: threading.Event, pulse_interval_s: float, arm_delay_ms: int):
    while not stop.is_set():
        try:
            require_ack(transmitter, 'ARM', str(arm_delay_ms), timeout=2.0)
        except (RuntimeError, TimeoutError):
            pass
        stop.wait(pulse_interval_s)


def run_one_attempt(card: Endpoint, transmitter, combo_index: int, sample_rate_hz: float,
                     sdr_duration_s: float, sdr_gain, nperseg: int,
                     pulsing: bool, pulse_interval_s: float, arm_delay_ms: int):
    before = card_status(card)
    if before.get('BPC') == '1':
        raise RuntimeError(f'BENCH_PASS_B_CAD already active before start: {before}')

    require_ack(card, 'BENCH_PASS_B_CAD', str(combo_index))

    stop = threading.Event()
    pulser = None
    if pulsing:
        pulser = threading.Thread(target=pulse_loop, args=(transmitter, stop, pulse_interval_s, arm_delay_ms),
                                   daemon=True)
        pulser.start()

    try:
        samples = capture_iq(CAD_TEST_FREQ_MHZ * 1e6, sample_rate_hz, sdr_duration_s, sdr_gain)
    finally:
        stop.set()
        if pulser is not None:
            pulser.join(timeout=2.0)

    deadline = time.monotonic() + WINDOW_TERMINAL_TIMEOUT_S
    while time.monotonic() < deadline:
        status = card_status(card)
        if status.get('BPC') == '0':
            break
        time.sleep(STATUS_POLL_INTERVAL_S)
    else:
        raise TimeoutError('BENCH_PASS_B_CAD did not complete after the SDR capture finished')

    opcode, payload = card.request('BENCH_PASS_B_CAD_RESULT', '-', timeout=3.0)
    if opcode != 'ACK':
        raise RuntimeError(f'BENCH_PASS_B_CAD_RESULT failed: {opcode} {payload}')
    result = payload.split('=', 1)[1] if '=' in payload else payload

    freqs, psd_db, freqs_spec, times_spec, sxx_db = analyze(
        samples, sample_rate_hz, CAD_TEST_FREQ_MHZ * 1e6, nperseg)
    center_idx = int(np.argmin(np.abs(freqs - CAD_TEST_FREQ_MHZ * 1e6)))
    return {
        'result': result,
        'sdr_peak_db_at_bin': float(psd_db[center_idx]),
        'sdr_peak_db_overall': float(np.max(psd_db)),
        'freqs': freqs, 'psd_db': psd_db,
        'freqs_spec': freqs_spec, 'times_spec': times_spec, 'sxx_db': sxx_db,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cardputer-port', required=True)
    parser.add_argument('--heltec-port', required=True)
    parser.add_argument('--combo-index', type=int, nargs='+', default=None,
                         help='row index/indices into PASS_B_SF_BW_CANDIDATES (pass_b_plan.h), '
                              '0-9. Repeatable/space-separated for more than one in the same '
                              'session (avoids a device reboot per combo -- opening a new serial '
                              'connection resets the ESP32-S3). Default: all 10, in order.')
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--pulse', action='store_true',
                         help='arm the Heltec during each attempt (positive control); '
                              'default is quiet-only, matching the false-positive question')
    parser.add_argument('--pulse-candidate', default='MESH_OREGON',
                         help='Heltec candidate to fire if --pulse (default matches '
                              'CAD_TEST_FREQ_MHZ, 918.5MHz)')
    parser.add_argument('--sdr-sample-rate-hz', type=float, default=2.4e6)
    parser.add_argument('--sdr-duration-s', type=float, default=3.5,
                         help='covers the worst case: DISCOVERY_CAD_TIMEOUT_MS (300ms) + a '
                              'possible DISCOVERY_RX_WINDOW_MS (2500ms) receive-on-hit, plus '
                              'margin for serial round-trip start latency')
    parser.add_argument('--sdr-gain', default='29.7')
    parser.add_argument('--nperseg', type=int, default=4096)
    parser.add_argument('--pulse-interval-s', type=float, default=0.2)
    parser.add_argument('--arm-delay-ms', type=int, default=80)
    parser.add_argument('--out-dir', default='captures')
    parser.add_argument('--results', default=None)
    args = parser.parse_args()

    combo_indices = args.combo_index if args.combo_index is not None else list(range(len(CANDIDATES)))
    for idx in combo_indices:
        if not 0 <= idx < len(CANDIDATES):
            parser.error(f'--combo-index values must be 0..{len(CANDIDATES) - 1}, got {idx}')
    gain = 'auto' if args.sdr_gain == 'auto' else float(args.sdr_gain)

    out_dir = pathlib.Path(__file__).parent / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    log_path = out_dir / f'cad-combos-{stamp}.serial.log'
    results = ResultWriter(args.results) if args.results else None

    with log_path.open('a', encoding='utf-8') as log:
        card = Endpoint('cardputer', args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            boot_identity = require_ack(card, 'HELLO', '-', timeout=15.0)
            card.record('BOOT_CONFIRMED ' + boot_identity)
            if 'BENCH=1' not in boot_identity:
                raise RuntimeError(
                    f'Cardputer is not running the bench image (BENCH_PASS_B_CAD needs it): {boot_identity}')
            if results:
                results.write({'event': 'boot', 'identity': boot_identity})

            if args.pulse:
                transmitter = Endpoint('heltec', args.heltec_port, TX_MARKER, log)
                require_ack(transmitter, 'HELLO', '-')
                require_ack(transmitter, 'QUIET', '-')
                require_ack(transmitter, 'CONFIG', args.pulse_candidate)

            combo_summaries = []
            for combo_index in combo_indices:
                sf, bw_khz = CANDIDATES[combo_index]
                print(f'\ncombo {combo_index}: SF{sf}/BW{bw_khz} @ {CAD_TEST_FREQ_MHZ}MHz, '
                      f'{"pulsing" if args.pulse else "quiet"}, {args.repeats} attempts')

                outcomes = []
                for i in range(args.repeats):
                    attempt = run_one_attempt(card, transmitter, combo_index, args.sdr_sample_rate_hz,
                                               args.sdr_duration_s, gain, args.nperseg,
                                               args.pulse, args.pulse_interval_s, args.arm_delay_ms)
                    outcomes.append(attempt['result'])
                    print(f'  [{i}] CAD={attempt["result"]:<12} '
                          f'SDR peak@bin={attempt["sdr_peak_db_at_bin"]:.1f}dB '
                          f'SDR peak overall={attempt["sdr_peak_db_overall"]:.1f}dB')

                    png_path = out_dir / f'cad-combo{combo_index}-{i}-{attempt["result"]}-{stamp}.png'
                    save_plot(png_path, attempt['freqs'], attempt['psd_db'], attempt['freqs_spec'],
                              attempt['times_spec'], attempt['sxx_db'], CAD_TEST_FREQ_MHZ * 1e6,
                              title=f'combo{combo_index} SF{sf}/BW{bw_khz} attempt {i}: '
                                    f'CAD={attempt["result"]}')

                    if results:
                        results.write({
                            'event': 'attempt', 'combo_index': combo_index, 'sf': sf, 'bw_khz': bw_khz,
                            'pulsing': args.pulse, 'attempt': i, 'cad_result': attempt['result'],
                            'sdr_peak_db_at_bin': attempt['sdr_peak_db_at_bin'],
                            'sdr_peak_db_overall': attempt['sdr_peak_db_overall'],
                        })

                detected = outcomes.count('cad_detected')
                print(f'  {detected}/{args.repeats} CAD_DETECTED')
                combo_summaries.append((combo_index, sf, bw_khz, detected, args.repeats))
                if results:
                    results.write({'event': 'combo_summary', 'combo_index': combo_index,
                                    'detected': detected, 'total': args.repeats, 'outcomes': outcomes})

            print(f'\n{"combo":<7} {"SF/BW":<12} {"detected":>10}')
            for combo_index, sf, bw_khz, detected, total in combo_summaries:
                print(f'{combo_index:<7} SF{sf}/BW{bw_khz:<8} {detected:>7}/{total}')
            print('\nCheck the saved PNGs for CAD_DETECTED attempts specifically -- a detected '
                  'result with a flat, featureless waterfall at 918.5MHz is a direct, visual '
                  'confirmation of a false trigger.')
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
