#!/usr/bin/env python3
"""Synchronized RTL-SDR + Cardputer capture: watch the same window two
independent instruments, at once, instead of comparing separate sessions.

Triggers the Cardputer's BENCH_RSSI_WINDOW (parks its own SX1262 at one
frequency and samples RSSI for ~2s -- see radio_task.cpp's
performBenchRssiWindow()), and starts an RTL-SDR capture of the same
frequency at essentially the same moment, while the Heltec fires repeated
pulses through the window. The two instruments use different, incompatible
scales -- the Cardputer's MAX/AVG are real calibrated dBm from the SX1262's
own RSSI register; the RTL-SDR's PSD is dB against an arbitrary reference,
not calibrated dBm -- so this does not (yet) produce one merged number.
What it does give: whether an independent receiver's waterfall shows real
energy at the same frequency, during the same window, that the Cardputer
reported a rise for -- a qualitative cross-check current bench sessions
don't have, run automatically instead of eyeballing two separate captures
taken minutes apart.

Timing note: the Cardputer's window starts when its BENCH_RSSI_WINDOW ACK
is received, not exactly when this script sends the request -- serial
round-trip latency (seen at ~10-50ms in this project's other bench
sessions) means the SDR capture (kicked off immediately after that ACK)
starts within roughly that same margin of the Cardputer's own window, not
exactly synchronized to it. Good enough to catch a ~2s window with a
capture a bit longer than that (see --sdr-duration-s), not a
hardware-triggered guarantee.

Requires the cardputer-adv-bench image on the Cardputer (BENCH_RSSI_WINDOW
is bench-only) and the Heltec transmitter bench sketch.
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

# Sourced from bench/heltec-v4r8-transmitter/src/main.cpp's own CANDIDATES
# table -- picking by name here (not a raw --center-freq-mhz) means the
# Heltec's CONFIG and the SDR's tuned frequency can't drift apart by typo.
CANDIDATE_FREQS_MHZ = {
    'LONG_MODERATE': 912.8125,
    'SHORT_FAST': 918.875,
    'SHORT_SLOW': 920.625,
    'MESH_OREGON': 918.5,
}

WINDOW_TERMINAL_TIMEOUT_S = 15.0
STATUS_POLL_INTERVAL_S = 0.1


def pulse_loop(transmitter: Endpoint, stop: threading.Event, pulse_interval_s: float, arm_delay_ms: int):
    while not stop.is_set():
        try:
            require_ack(transmitter, 'ARM', str(arm_delay_ms), timeout=2.0)
        except (RuntimeError, TimeoutError):
            pass  # a transient miss here just costs one fewer pulse this window
        stop.wait(pulse_interval_s)


def run_synced_window(card: Endpoint, transmitter: Endpoint, center_freq_hz: float,
                       sample_rate_hz: float, sdr_duration_s: float, sdr_gain,
                       nperseg: int, pulsing: bool, pulse_interval_s: float, arm_delay_ms: int):
    before = card_status(card)
    if before.get('RW') == '1':
        raise RuntimeError(f'RSSI window already active before start: {before}')

    freq_khz = int(round(center_freq_hz / 1000.0))
    require_ack(card, 'BENCH_RSSI_WINDOW', str(freq_khz))
    card.record(f'SYNC_START pulsing={pulsing}')

    stop = threading.Event()
    pulser = None
    if pulsing:
        pulser = threading.Thread(target=pulse_loop, args=(transmitter, stop, pulse_interval_s, arm_delay_ms),
                                   daemon=True)
        pulser.start()

    try:
        samples = capture_iq(center_freq_hz, sample_rate_hz, sdr_duration_s, sdr_gain)
    finally:
        stop.set()
        if pulser is not None:
            pulser.join(timeout=2.0)

    deadline = time.monotonic() + WINDOW_TERMINAL_TIMEOUT_S
    while time.monotonic() < deadline:
        status = card_status(card)
        if status.get('RW') == '0':
            break
        time.sleep(STATUS_POLL_INTERVAL_S)
    else:
        raise TimeoutError('Cardputer RSSI window did not complete after the SDR capture finished')

    opcode, payload = card.request('BENCH_RSSI_RESULT', '-', timeout=3.0)
    if opcode != 'ACK':
        raise RuntimeError(f'BENCH_RSSI_RESULT failed: {opcode} {payload}')
    fields = {}
    for item in payload.split(';'):
        key, sep, value = item.partition('=')
        if sep:
            fields[key] = value
    card_max_dbm = int(fields['MAX']) / 10.0
    card_avg_dbm = int(fields.get('AVG', '0')) / 10.0

    freqs, psd_db, freqs_spec, times_spec, sxx_db = analyze(samples, sample_rate_hz, center_freq_hz, nperseg)
    return {
        'card_max_dbm': card_max_dbm, 'card_avg_dbm': card_avg_dbm,
        'freqs': freqs, 'psd_db': psd_db,
        'freqs_spec': freqs_spec, 'times_spec': times_spec, 'sxx_db': sxx_db,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cardputer-port', required=True)
    parser.add_argument('--heltec-port', required=True)
    parser.add_argument('--candidate', choices=sorted(CANDIDATE_FREQS_MHZ), default='SHORT_SLOW')
    parser.add_argument('--sdr-sample-rate-hz', type=float, default=2.4e6)
    parser.add_argument('--sdr-duration-s', type=float, default=3.0,
                         help='should comfortably bracket the Cardputer\'s own fixed ~2s window '
                              '(BENCH_RSSI_WINDOW_SAMPLE_COUNT * _INTERVAL_MS in radio_task.cpp) '
                              'given the timing note above -- default 3.0s')
    parser.add_argument('--sdr-gain', default='29.7',
                         help='manual gain in dB, or "auto". Fixed gain matched across the '
                              'quiet and pulsing runs matters here for the same reason it did '
                              'for capture_spectrum.py -- see its own --gain help.')
    parser.add_argument('--nperseg', type=int, default=4096)
    parser.add_argument('--pulse-interval-s', type=float, default=0.2)
    parser.add_argument('--arm-delay-ms', type=int, default=80)
    parser.add_argument('--out-dir', default='captures')
    parser.add_argument('--log', default=None, help='raw serial capture (default: alongside --out-dir)')
    parser.add_argument('--results', default=None, help='append-only JSONL scenario results')
    args = parser.parse_args()

    gain = 'auto' if args.sdr_gain == 'auto' else float(args.sdr_gain)
    center_freq_hz = CANDIDATE_FREQS_MHZ[args.candidate] * 1e6

    out_dir = pathlib.Path(__file__).parent / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    log_path = pathlib.Path(args.log) if args.log else out_dir / f'sync-{args.candidate}-{stamp}.serial.log'
    results = ResultWriter(args.results) if args.results else None

    with log_path.open('a', encoding='utf-8') as log:
        card = Endpoint('cardputer', args.cardputer_port, CARD_MARKER, log)
        transmitter = None
        try:
            boot_identity = require_ack(card, 'HELLO', '-', timeout=15.0)
            card.record('BOOT_CONFIRMED ' + boot_identity)
            if 'BENCH=1' not in boot_identity:
                raise RuntimeError(
                    f'Cardputer is not running the bench image (BENCH_RSSI_WINDOW needs it): {boot_identity}')
            if results:
                results.write({'event': 'boot', 'identity': boot_identity})

            transmitter = Endpoint('heltec', args.heltec_port, TX_MARKER, log)
            require_ack(transmitter, 'HELLO', '-')
            require_ack(transmitter, 'QUIET', '-')
            require_ack(transmitter, 'CONFIG', args.candidate)

            print(f'quiet window at {args.candidate} ({center_freq_hz/1e6}MHz), '
                  f'SDR {args.sdr_duration_s}s @ gain={gain}...')
            quiet = run_synced_window(card, transmitter, center_freq_hz, args.sdr_sample_rate_hz,
                                       args.sdr_duration_s, gain, args.nperseg,
                                       pulsing=False, pulse_interval_s=args.pulse_interval_s,
                                       arm_delay_ms=args.arm_delay_ms)

            print(f'pulsing window at {args.candidate} ({center_freq_hz/1e6}MHz), '
                  f'SDR {args.sdr_duration_s}s @ gain={gain}...')
            pulse = run_synced_window(card, transmitter, center_freq_hz, args.sdr_sample_rate_hz,
                                       args.sdr_duration_s, gain, args.nperseg,
                                       pulsing=True, pulse_interval_s=args.pulse_interval_s,
                                       arm_delay_ms=args.arm_delay_ms)
            require_ack(transmitter, 'QUIET', '-')

            card_delta_db = pulse['card_max_dbm'] - quiet['card_max_dbm']
            # SDR delta at the exact target bin (nearest freq index to
            # center) -- the PSD's own scale is uncalibrated/arbitrary, so
            # only the *relative* rise (pulsing minus quiet, same
            # instrument, same bin) is meaningful, not either absolute
            # number on its own or a comparison to the Cardputer's dBm.
            center_idx = int(np.argmin(np.abs(quiet['freqs'] - center_freq_hz)))
            sdr_quiet_db = float(quiet['psd_db'][center_idx])
            sdr_pulse_db = float(pulse['psd_db'][center_idx])
            sdr_delta_db = sdr_pulse_db - sdr_quiet_db

            print(f'\nCardputer (calibrated dBm): quiet={quiet["card_max_dbm"]:.1f}dBm '
                  f'pulse={pulse["card_max_dbm"]:.1f}dBm delta={card_delta_db:+.1f}dB')
            print(f'RTL-SDR (arbitrary-ref dB, at target bin): quiet={sdr_quiet_db:.1f}dB '
                  f'pulse={sdr_pulse_db:.1f}dB delta={sdr_delta_db:+.1f}dB')
            both_saw_it = card_delta_db > 10.0 and sdr_delta_db > 3.0
            print(f'both instruments saw a rise at this frequency during this window: {both_saw_it}')

            for label, data in (('quiet', quiet), ('pulse', pulse)):
                png_path = out_dir / f'sync-{args.candidate}-{label}-{stamp}.png'
                save_plot(png_path, data['freqs'], data['psd_db'], data['freqs_spec'],
                          data['times_spec'], data['sxx_db'], center_freq_hz,
                          title=f'{args.candidate} {center_freq_hz/1e6}MHz {label} '
                                f'(Cardputer MAX={data["card_max_dbm"]:.1f}dBm)')
                print(f'saved: {png_path}')

            if results:
                results.write({
                    'event': 'summary', 'candidate': args.candidate, 'center_freq_hz': center_freq_hz,
                    'card_quiet_dbm': quiet['card_max_dbm'], 'card_pulse_dbm': pulse['card_max_dbm'],
                    'card_delta_db': card_delta_db,
                    'sdr_quiet_db': sdr_quiet_db, 'sdr_pulse_db': sdr_pulse_db, 'sdr_delta_db': sdr_delta_db,
                    'both_saw_it': both_saw_it,
                })
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
