#!/usr/bin/env python3
"""RTL-SDR spectrum capture + PSD/waterfall bench tool.

Independent verification for this project's own RX-path bench claims: the
SX1262 is the instrument under test in every other bench script here, so
it can't independently confirm what's actually present in the RF
environment. That mattered concretely once already -- the 923MHz-edge
rolloff investigation (docs/STATUS.md) initially misread a real ~24dB
quiet-baseline difference between two frequencies as a receiver
sensitivity gap, because the only instrument available was the SX1262
itself, reporting a single RSSI number with no spectral shape behind it.
An RTL-SDR capture answers "what does the RF environment actually look
like at this frequency" directly, independent of the Cardputer's own
receiver.

Developed against an RTL-SDR Blog V4. Requires the stock kernel DVB-T
driver (dvb_usb_rtl28xxu) blacklisted so librtlsdr can open the device --
see README.md. RX only, same as the rest of this project.
"""

import argparse
import pathlib
import sys
from datetime import datetime, timezone

import numpy as np
from scipy import signal

# pyrtlsdr's synchronous read_samples() does one libusb bulk transfer per
# call, no internal chunking. 2**18 (~262k samples) is commonly cited as a
# safe chunk size but overflowed (LIBUSB_ERROR_OVERFLOW) on this exact
# RTL-SDR Blog V4 + rebuilt rtl-sdr-blog driver combination -- 2**14 (16384
# samples, 32768 bytes) is the smaller, more conservative size seen across
# most working pyrtlsdr examples and is what actually worked here.
READ_CHUNK_SAMPLES = 1 << 14

# Settle time after retuning before samples are kept: the tuner PLL and (if
# gain='auto') the AGC both need to stabilize, and a cold read right after
# center_freq/gain changes is a known source of transient garbage in
# RTL-SDR captures.
SETTLE_SECONDS = 0.15


# RTL-SDR's underlying USB bulk transfer requires the requested byte count
# to be a multiple of 512 -- a non-aligned read (e.g. an oddly-sized final
# "remainder" chunk) raises LIBUSB_ERROR_OVERFLOW, not a size or timing
# problem as it first appears. 256 samples = 512 bytes (2 bytes/sample).
ALIGN_SAMPLES = 256


def _read_chunked(sdr, num_samples: int):
    # pyrtlsdr's read_samples() does one synchronous libusb transfer per
    # call with no internal chunking -- keep individual requests bounded
    # so a whole settle-flush or capture duration isn't attempted as one
    # giant transfer. Every request (including the final, otherwise-
    # ragged remainder) is rounded up to a 512-byte-aligned sample count;
    # the caller gets back at least num_samples and trims any small
    # overshoot itself. Note: pyrtlsdr's read_bytes() calls self.close()
    # on ANY read error before raising, so a failed read leaves the
    # device pointer dead -- retrying reads on the same sdr object after
    # a failure segfaults (use-after-free in librtlsdr), it does not
    # recover, so getting the alignment right up front matters more than
    # it would with an error path that could just be retried.
    chunks = []
    remaining = num_samples
    while remaining > 0:
        n = min(READ_CHUNK_SAMPLES, remaining)
        if n % ALIGN_SAMPLES != 0:
            n = ((n // ALIGN_SAMPLES) + 1) * ALIGN_SAMPLES
        chunks.append(sdr.read_samples(n))
        remaining -= n
    return chunks


def capture_iq(center_freq_hz: float, sample_rate_hz: float, duration_s: float, gain):
    from rtlsdr import RtlSdr  # imported here so --help works without the driver installed

    sdr = RtlSdr()
    try:
        sdr.sample_rate = sample_rate_hz
        sdr.center_freq = center_freq_hz
        sdr.gain = gain
        # Retuning sample_rate/center_freq/gain after open() leaves stale
        # data in librtlsdr's internal ring buffer without another
        # explicit reset (open() only resets it once, before these are
        # set) -- the first read otherwise reliably raises
        # LIBUSB_ERROR_OVERFLOW on this device/driver combination,
        # regardless of chunk size, and pyrtlsdr's read path closes (not
        # just fails) the device on any read error, so this must be
        # avoided rather than retried.
        from rtlsdr.librtlsdr import librtlsdr
        librtlsdr.rtlsdr_reset_buffer(sdr.dev_p)
        _read_chunked(sdr, int(sample_rate_hz * SETTLE_SECONDS))  # discard, just flushes/settles

        total = int(sample_rate_hz * duration_s)
        # _read_chunked's alignment rounding can overshoot total by up to
        # ALIGN_SAMPLES-1 on the last chunk; trim back to the exact count
        # the caller asked for.
        return np.concatenate(_read_chunked(sdr, total))[:total]
    finally:
        sdr.close()


def analyze(samples: np.ndarray, sample_rate_hz: float, center_freq_hz: float, nperseg: int):
    # Welch PSD: an averaged, less noisy spectral estimate than a single
    # FFT -- what we actually want for "is this frequency quiet or busy",
    # not a single noisy snapshot.
    freqs, psd = signal.welch(samples, fs=sample_rate_hz, nperseg=nperseg,
                               return_onesided=False, scaling='density')
    freqs = np.fft.fftshift(freqs) + center_freq_hz
    psd_db = 10 * np.log10(np.fft.fftshift(psd) + 1e-20)

    # Spectrogram (time x frequency): distinguishes a steady broadband
    # noise floor from something that hops or pulses -- the exact
    # distinction needed to tell self-noise from a real intermittent
    # transmitter (e.g. frequency-hopping AMI/smart-meter traffic).
    freqs_spec, times_spec, sxx = signal.spectrogram(
        samples, fs=sample_rate_hz, nperseg=nperseg, return_onesided=False)
    freqs_spec = np.fft.fftshift(freqs_spec) + center_freq_hz
    sxx_db = 10 * np.log10(np.fft.fftshift(sxx, axes=0) + 1e-20)

    return freqs, psd_db, freqs_spec, times_spec, sxx_db


def save_plot(path: pathlib.Path, freqs, psd_db, freqs_spec, times_spec, sxx_db,
              center_freq_hz: float, title: str):
    import matplotlib
    matplotlib.use('Agg')  # headless -- this runs over SSH/serial-bench sessions, no display
    import matplotlib.pyplot as plt

    fig, (ax_psd, ax_wf) = plt.subplots(2, 1, figsize=(10, 8), height_ratios=[1, 2])
    fig.suptitle(title)

    ax_psd.plot((freqs - center_freq_hz) / 1e3, psd_db, linewidth=0.8)
    ax_psd.set_ylabel('PSD (dB, arbitrary ref)')
    ax_psd.set_xlabel('offset from center (kHz)')
    ax_psd.grid(True, alpha=0.3)

    extent = [times_spec[0], times_spec[-1],
              (freqs_spec[0] - center_freq_hz) / 1e3, (freqs_spec[-1] - center_freq_hz) / 1e3]
    im = ax_wf.imshow(sxx_db, aspect='auto', origin='lower', extent=extent, cmap='viridis')
    ax_wf.set_ylabel('offset from center (kHz)')
    ax_wf.set_xlabel('time (s)')
    fig.colorbar(im, ax=ax_wf, label='dB')

    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)


def _parse_gain(text: str):
    # pyrtlsdr's gain setter branches on the *type* it receives (the
    # literal string 'auto' enables AGC; anything else must already be
    # numeric) -- argparse hands every value through as str, so "29.7"
    # has to become a float here or set_gain's own arithmetic on it fails.
    if text == 'auto':
        return text
    return float(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--center-freq-mhz', type=float, required=True)
    parser.add_argument('--sample-rate-hz', type=float, default=2.4e6,
                         help='2.4MSPS is the conventional stable rate for RTL-SDR devices')
    parser.add_argument('--duration-s', type=float, default=10.0)
    parser.add_argument('--gain', type=_parse_gain, default='auto',
                         help='"auto" for AGC, or a manual gain in dB (e.g. "29.7"). '
                              'AGC is a reasonable first-pass default, but comparing two '
                              'different frequencies\' absolute levels needs a matched manual '
                              'gain on both captures -- AGC free to differ between them '
                              'reintroduces exactly the "unequal baseline" trap docs/STATUS.md '
                              'already documents once for this same 920MHz question.')
    parser.add_argument('--nperseg', type=int, default=4096,
                         help='FFT segment size: frequency resolution = sample_rate/nperseg')
    parser.add_argument('--out-dir', default='captures')
    parser.add_argument('--label', default='',
                         help='short tag included in the output filename, e.g. "edge-quiet"')
    parser.add_argument('--save-spectrogram-array', action='store_true',
                         help='also save the dense waterfall array to the .npz, not just the '
                              'PNG render of it. Off by default: it\'s a full N-sample-sized '
                              'grid (~50-100MB even at float32 for a 5s/2.4MSPS capture), unlike '
                              'the averaged PSD (tens of KB) that\'s always saved.')
    parser.add_argument('--save-iq', action='store_true',
                         help='also save the raw complex64 IQ samples (large: '
                              'sample_rate * duration * 8 bytes -- ~192MB for the defaults at '
                              '10s). Off by default.')
    args = parser.parse_args()

    out_dir = pathlib.Path(__file__).parent / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    tag = f'{args.label}-' if args.label else ''
    # Plain string base, not pathlib's with_suffix() -- the center-freq
    # float (e.g. "920.6250MHz") contains its own '.', which with_suffix()
    # mistakes for the filename's real extension and mangles the name.
    base_name = f'{tag}{args.center_freq_mhz:.4f}MHz-{stamp}'

    center_freq_hz = args.center_freq_mhz * 1e6
    print(f'capturing {args.duration_s}s at {args.center_freq_mhz}MHz, '
          f'{args.sample_rate_hz/1e6:.2f}MSPS, gain={args.gain}...')
    samples = capture_iq(center_freq_hz, args.sample_rate_hz, args.duration_s, args.gain)

    freqs, psd_db, freqs_spec, times_spec, sxx_db = analyze(
        samples, args.sample_rate_hz, center_freq_hz, args.nperseg)

    npz_path = out_dir / f'{base_name}.npz'
    # Default save is just the averaged PSD (freq axis + one spectrum,
    # tens of KB) -- genuinely small and safe to commit if a finding is
    # worth keeping. The dense spectrogram is a full N-sample-sized grid
    # (a 5s/2.4MSPS capture is already ~50-100MB even at float32), so it's
    # only ever rendered into the PNG unless explicitly asked for.
    save_kwargs = dict(
        freqs=freqs.astype(np.float32), psd_db=psd_db.astype(np.float32),
        center_freq_hz=center_freq_hz, sample_rate_hz=args.sample_rate_hz,
        gain=str(args.gain), duration_s=args.duration_s,
    )
    if args.save_spectrogram_array:
        save_kwargs['freqs_spec'] = freqs_spec.astype(np.float32)
        save_kwargs['times_spec'] = times_spec.astype(np.float32)
        save_kwargs['sxx_db'] = sxx_db.astype(np.float32)
    if args.save_iq:
        save_kwargs['iq'] = samples
    np.savez_compressed(npz_path, **save_kwargs)

    png_path = out_dir / f'{base_name}.png'
    save_plot(png_path, freqs, psd_db, freqs_spec, times_spec, sxx_db, center_freq_hz,
              title=f'{args.center_freq_mhz}MHz, {args.duration_s}s, gain={args.gain}')

    peak_db = float(np.max(psd_db))
    mean_db = float(np.mean(psd_db))
    peak_offset_khz = float((freqs[int(np.argmax(psd_db))] - center_freq_hz) / 1e3)
    print(f'mean PSD: {mean_db:.1f}dB, peak PSD: {peak_db:.1f}dB '
          f'at {peak_offset_khz:+.1f}kHz offset')
    print(f'saved: {npz_path}')
    print(f'saved: {png_path}')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
