#!/usr/bin/env python3
"""Compare two or more RTL-SDR spectrum captures (.npz from capture_spectrum.py)
side by side -- an overlaid PSD plot plus a printed summary, instead of the
one-off Python snippets this project's own bench sessions were using to do
the same comparison by hand.
"""

import argparse
import pathlib
import sys

import numpy as np


def load_capture(path: pathlib.Path):
    data = np.load(path)
    return {
        'path': path,
        'freqs': data['freqs'],
        'psd_db': data['psd_db'],
        'center_freq_hz': float(data['center_freq_hz']),
        'sample_rate_hz': float(data['sample_rate_hz']),
        'gain': str(data['gain'].item()),
        'duration_s': float(data['duration_s']),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('captures', nargs='+', type=pathlib.Path,
                         help='.npz files from capture_spectrum.py (2 or more)')
    parser.add_argument('--by', choices=['offset', 'absolute'], default='offset',
                         help='"offset": x-axis is offset from each capture\'s own center '
                              'frequency -- compares spectral shape/character across captures '
                              'taken at different frequencies (e.g. mid-band vs edge-band). '
                              '"absolute": x-axis is real frequency -- compares the same '
                              'frequency across different capture sessions (e.g. checking for '
                              'drift/repeatability). Default: offset.')
    parser.add_argument('--label', action='append', default=[],
                         help='override label for a capture, in the order given on the command '
                              'line (default: filename stem). Repeat for multiple captures.')
    parser.add_argument('--out', type=pathlib.Path, default=None,
                         help='PNG output path (default: alongside the first capture)')
    args = parser.parse_args()

    if len(args.captures) < 2:
        parser.error('need at least 2 captures to compare')

    captures = [load_capture(p) for p in args.captures]
    labels = list(args.label) + [c['path'].stem for c in captures[len(args.label):]]

    # Comparing absolute levels across different gain/AGC settings
    # reintroduces exactly the "unequal baseline" trap docs/STATUS.md
    # already documents once for this same kind of question (the SX1262
    # rolloff investigation's first, misleading "rise from quiet" number).
    gains = {c['gain'] for c in captures}
    if len(gains) > 1:
        print(f'WARNING: captures use different gain settings ({sorted(gains)}) -- an '
              'absolute-level comparison between them is not meaningful (AGC in particular '
              'can settle to a different point on each capture). Re-capture with a matching '
              '--gain on all of them for a comparison that means anything.', file=sys.stderr)

    print(f'{"label":<20} {"center":>12} {"gain":>8} {"dur":>6} {"mean dB":>9} {"peak dB":>9} {"peak @":>10}')
    for cap, label in zip(captures, labels):
        peak_idx = int(np.argmax(cap['psd_db']))
        peak_offset_khz = (cap['freqs'][peak_idx] - cap['center_freq_hz']) / 1e3
        print(f'{label:<20} {cap["center_freq_hz"]/1e6:>10.4f}M {cap["gain"]:>8} '
              f'{cap["duration_s"]:>5.1f}s {np.mean(cap["psd_db"]):>9.1f} '
              f'{np.max(cap["psd_db"]):>9.1f} {peak_offset_khz:>+9.1f}k')

    if len(captures) == 2:
        a, b = captures
        print(f'\nmean PSD delta ({labels[0]} - {labels[1]}): '
              f'{np.mean(a["psd_db"]) - np.mean(b["psd_db"]):+.1f}dB')

    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(10, 5))
    for cap, label in zip(captures, labels):
        if args.by == 'offset':
            x = (cap['freqs'] - cap['center_freq_hz']) / 1e3
        else:
            x = cap['freqs'] / 1e6
        ax.plot(x, cap['psd_db'], linewidth=0.8, label=f'{label} (gain={cap["gain"]})', alpha=0.85)

    ax.set_xlabel('offset from center (kHz)' if args.by == 'offset' else 'frequency (MHz)')
    ax.set_ylabel('PSD (dB, arbitrary ref)')
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    out_path = args.out or (captures[0]['path'].parent /
                             f'compare-{"-".join(l.replace(" ", "_") for l in labels)}.png')
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    print(f'\nsaved: {out_path}')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
