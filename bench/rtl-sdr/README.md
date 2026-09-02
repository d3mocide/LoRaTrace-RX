# RTL-SDR spectrum bench tool

Independent RF-environment verification for this project's own bench work.
Every other bench tool here (`bench/heltec-v4r8-transmitter`,
`scripts/phase*.py`) measures the RF environment through the Cardputer's
own SX1262 receiver -- which is the instrument under test, not a neutral
observer. That gap was concrete, not theoretical: the 923MHz-edge rolloff
investigation (`docs/STATUS.md`) initially read a real ~24dB difference in
*ambient* noise between 912.8MHz and 920.6MHz as a receiver sensitivity
problem, because the SX1262's own RSSI was the only instrument available
and it reports one number, not a spectrum. An RTL-SDR capture answers "what
is actually present in the RF environment at this frequency" directly.

Developed against an RTL-SDR Blog V4 (`0bda:2838`) with a Muzi Works
915MHz whip -- the same antenna model used on the Cardputer and Heltec
nodes, so a capture here is looking at the same RF environment those
devices see, not a different one.

RX only, same as the rest of this project. This is a receive-only tool;
it has no transmit capability at all.

## One-time setup

The stock kernel DVB-T driver claims RTL-SDR dongles by default (they're
TV-tuner chips) and has to be kept off the device before `librtlsdr` can
open it in raw I/Q mode. The V4 also needs RTL-SDR Blog's own driver fork,
not the stock osmocom package -- it has V4-specific fixes stock `rtl-sdr`
doesn't:

```bash
sudo pacman -S python-numpy python-scipy python-matplotlib
yay -S rtl-sdr-blog   # conflicts with stock rtl-sdr; let it remove that package
echo "blacklist dvb_usb_rtl28xxu" | sudo tee /etc/modprobe.d/blacklist-rtl-sdr.conf
sudo rmmod dvb_usb_rtl28xxu   # unbind for this session; the blacklist file covers future boots
```

Verify the device is reachable (the "No E4000 tuner found, aborting" line
afterward is normal -- `rtl_test` just finished probing tuner types and
already found the real one, R828D, above it):

```bash
rtl_test -t
```

Then set up the Python venv (`--system-site-packages` so it reuses the
numpy/scipy/matplotlib installed above):

```bash
./setup.sh
```

This also patches pyrtlsdr's bindings: as of 2026-09, PyPI's `pyrtlsdr`
(and the AUR `-git` version) bind several dithering/GPIO functions
(`rtlsdr_set_dithering` etc.) that neither stock `rtl-sdr` nor
`rtl-sdr-blog`'s librtlsdr actually export yet -- an unguarded attribute
access that crashes the import even though this tool never calls any of
them. `setup.sh` wraps that block in the same `try/except AttributeError`
pattern the file's own author already uses one function above it, and is
safe to re-run (it's idempotent, and re-creating `.venv/` from scratch
reapplies the same patch). If a future `pyrtlsdr` release adds real
support for these on this hardware, `setup.sh`'s patch step will start
printing "already patched, skipping" against the *old* bindings file it's
patching, and will need revisiting.

## Usage

```bash
.venv/bin/python capture_spectrum.py --center-freq-mhz 920.625 --duration-s 15 --gain 29.7 --label edge
.venv/bin/python capture_spectrum.py --center-freq-mhz 912.8125 --duration-s 15 --gain 29.7 --label mid
.venv/bin/python compare_spectrum.py captures/edge-*.npz captures/mid-*.npz --label edge --label mid
```

Each capture saves a `.npz` (frequency axis + averaged PSD -- tens of KB,
small and safe to commit if a finding is worth keeping) and a `.png` (PSD
line plot + waterfall image, rendered from the full-resolution spectrogram
but not saved as raw data by default -- see `--save-spectrogram-array` and
`--save-iq` if you need the underlying arrays for offline reanalysis; both
are large, tens to hundreds of MB, and off by default) under `captures/`
(gitignored).

`compare_spectrum.py` loads two or more `.npz` captures, prints a summary
table (mean/peak PSD, peak offset, and the pairwise mean delta for exactly
two captures), and saves an overlaid PSD plot. It warns if the captures
used different `--gain` settings, since an absolute-level comparison
between them wouldn't mean anything then (see the gain note below). `--by
offset` (default) compares spectral shape across different frequencies
(e.g. mid-band vs edge-band, aligned on offset-from-center); `--by
absolute` compares real frequency across different sessions (e.g.
checking whether the same frequency drifted or repeats cleanly).

`sync_capture.py` watches the *same window* with two independent
instruments at once, instead of comparing separate sessions: it triggers
the Cardputer's bench-only `BENCH_RSSI_WINDOW` (parks the SX1262 at one
frequency, samples RSSI for ~2s) and starts a matching RTL-SDR capture at
essentially the same moment, with the Heltec firing repeated pulses
through the window (a quiet run first, then a pulsing run, both saved as
PNGs). Needs the `cardputer-adv-bench` image on the Cardputer:

```bash
.venv/bin/python sync_capture.py \
  --cardputer-port /dev/ttyACM0 --heltec-port /dev/ttyACM1 \
  --candidate SHORT_SLOW --sdr-gain 29.7
```

First real run (2026-09-02, `SHORT_SLOW`/920.625MHz): the Cardputer's own
calibrated RSSI delta (quiet -73.0dBm to pulse -30.0dBm, **+43.0dB**) and
the RTL-SDR's independent, differently-calibrated PSD delta at the same
bin (**+43.2dB**) agreed to within 0.2dB -- two completely different
receivers, different hardware, different scales, agreeing on the size of
the same real event. The pulse waterfall shows every one of the ~12
Heltec pulses as a distinct ~250kHz-wide band (matching `SHORT_SLOW`'s
real BW) at the exact target frequency; the quiet waterfall shows nothing
there but faint periodic single-line spurs, unrelated to the pulses'
timing and almost certainly RTL-SDR-internal, not real RF. Real,
independent cross-validation of the Cardputer's own RSSI calibration.

**`--gain`** defaults to `auto` (AGC) for a first look at a frequency. If
comparing *absolute* levels between two different frequencies -- which is
exactly the kind of question this tool exists to answer -- use the same
fixed manual gain (e.g. `--gain 29.7`) on both captures. Letting AGC settle
independently on each capture reintroduces the same "unequal baseline"
trap `docs/STATUS.md` already documents once for this exact 920MHz
question, just one layer further out.

The waterfall (not just the averaged PSD) is the point for questions like
"is this a steady noise floor or something intermittent/hopping" -- e.g.
distinguishing self-generated broadband noise from AMI/smart-meter traffic
(commonly frequency-hopping within 900-928MHz in dense urban deployments,
a live hypothesis for LoRaTrace's own 920MHz observations), which would
show as discrete, moving lines over time rather than a flat elevated floor.

## What a real comparison found (2026-09-01)

`gain=auto` (AGC) is a fine first look, but letting AGC settle
independently on two different captures reintroduces the exact "unequal
baseline" trap that first made the SX1262 rolloff investigation misread a
real quiet-baseline difference as a sensitivity gap -- it happened here
too, on the very first AGC capture at 920.625MHz, which showed what looked
like a persistent elevated noise hump. A matched-gain (`--gain 29.7`)
capture at both 912.8125MHz and 920.625MHz came back nearly identical
(mean PSD -103.0dB vs -103.5dB, 0.4dB apart) and the "persistent hump"
was gone -- it was a brief transient in the fixed-gain capture, not a
steady condition. A 60-second matched-gain capture at 920.625MHz found
only three brief transient events in the full minute, otherwise flat --
not consistent with a densely-active or continuously-hopping signal at
that frequency. This independently corroborates the SX1262-based finding
in `docs/STATUS.md` (no real front-end rolloff, no persistent 920MHz
noise source) using a completely different instrument.

## Where this could go next

- LoRa chirp demodulation from raw IQ (`gr-lora-git`/`gr-lora_sdr-git` are
  available in the AUR) -- ground truth for `fingerprint.h`'s still-unbuilt
  post-hoc protocol classification (`CLAUDE.md`'s proposed layout), and a
  way to verify what a Sweep peak flagged `unknown_lora_candidate` actually
  is, independent of the Cardputer's own CAD result.
- Extend `sync_capture.py` alongside a real `scripts/phase9_*_bench.py`
  Sweep run (not just the single-frequency `BENCH_RSSI_WINDOW` it uses
  today), so a full Sweep and an RTL-SDR waterfall of the same window can
  be compared bin-by-bin.
- A web panel, once there's an actual library of captures to browse/
  compare or a need to watch a long-running capture live -- premature
  before that; the PNG output already covers a single-capture look.

`capture_spectrum.py`, `compare_spectrum.py`, and `sync_capture.py` are
the first slices.
