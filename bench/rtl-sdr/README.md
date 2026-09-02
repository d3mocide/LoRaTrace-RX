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

## Pass B CAD ground truth (2026-09-02)

`sync_cad_capture.py` triggers the Cardputer's bench-only `BENCH_PASS_B_CAD`
(one CAD attempt at a chosen `PASS_B_SF_BW_CANDIDATES` combo, fixed
918.5MHz test point) and starts a matching RTL-SDR capture at the same
moment, reading back the raw result via the new `BENCH_PASS_B_CAD_RESULT`
opcode (`bench_fault.h`/`.cpp`, `radio_task.cpp` -- previously this result
was only visible by pulling `energy.csv` off the SD card). Needs the
`cardputer-adv-bench` image:

```bash
.venv/bin/python sync_cad_capture.py \
  --cardputer-port /dev/ttyACM0 --heltec-port /dev/ttyACM1 \
  --combo-index 0 1 2 3 4 5 6 7 8 9 --repeats 5 --sdr-gain 29.7   # all 10, one session
```

`--combo-index` takes one or more indices in the same session (opening a
new serial connection resets the ESP32-S3, so looping combos inside one
script run avoids a reboot per combo); omit it to run all 10.

All 10 `PASS_B_SF_BW_CANDIDATES` combos, quiet condition, n=5 each
(2026-09-02):

| combo | SF/BW | confidence (pass_b_plan.h) | CAD_DETECTED |
|---|---|---|---|
| 0 | SF7/62.5 | unverified | 0/5 |
| 1 | SF7/250 | unverified | 0/5 |
| 2 | SF8/125 | **STRONG** | 0/5 |
| 3 | SF8/250 | unverified | 0/5 |
| 4 | SF9/250 | unverified | 0/5 |
| 5 | SF10/250 | unverified | 1/5 |
| 6 | SF11/125 | unverified | 0/5 |
| 7 | SF11/250 | unverified | 0/5 |
| 8 | SF11/500 | **NOISY** | 4/5 |
| 9 | SF12/125 | unverified | 1/5 |

Every single `CAD_DETECTED` across all ten combos (6 total instances) had
the same signature on the SDR's simultaneous waterfall: completely flat
at 918.5MHz, no signal, same ambient floor as every quiet attempt. This
directly confirms what `docs/research/phase9-sweep-pass-b-design.md`'s
own bench matrix could only infer from aggregate quiet-vs-pulse counts
(a false-positive rate that scales with SF/symbol-duration, i.e. the
radio false-triggering on ambient energy at long dwell times, not a real
signal): an independent receiver watching the exact moment confirms there
was genuinely nothing there, for every combo tested, not just the
noisiest one. `pass_b_plan.h`'s own standing comment -- "SX1262
CAD-at-arbitrary-bin behavior isn't verified" -- now has real, direct
(not just statistical) evidence behind it across the whole table. The
relative rates here (combo 8 far more prone to false triggering than any
other, combos 5/9 showing a low but nonzero rate, everything else clean
at n=5) are consistent with the original matrix's own numbers, not a
contradiction -- this is corroboration through a different method, not a
separate calibration (n=5/combo is far too small to recalibrate the
`STRONG`/`NOISY` rates themselves, which still rest on the original
1,200-cycle matrix).

**Positive control, `--pulse` (2026-09-02):** arms the Heltec once at
0ms delay immediately before each CAD trigger (the same precise
sequencing `scripts/phase9_pass_b_cad_bench.py`'s own `run_cycle`
established -- CAD's own scan window is only ~300ms, far tighter than
`BENCH_RSSI_WINDOW`'s ~2s, so a periodic pulse train mostly misses it;
that was this script's own first, unsuccessful attempt). All 10 combos,
`--pulse-candidate MESH_OREGON` (SF8/BW125, the exact match for combo 2),
n=5 each:

| combo | SF/BW | CAD_DETECTED (pulsing) |
|---|---|---|
| 0 | SF7/62.5 | 0/5 |
| 1 | SF7/250 | 0/5 |
| 2 | SF8/125 (exact match) | **5/5** |
| 3 | SF8/250 | 0/5 |
| 4 | SF9/250 | **5/5** |
| 5 | SF10/250 | **5/5** |
| 6 | SF11/125 | 0/5 |
| 7 | SF11/250 | 0/5 |
| 8 | SF11/500 | **5/5** |
| 9 | SF12/125 | 0/5 |

Matches the original bench matrix's own pulse-condition pattern exactly:
the exact-match combo plus the three whose bandwidth is a superset of
MESH_OREGON's 125kHz (SF9/250, SF10/250, SF11/500) all detect reliably;
the rest barely register the real pulse, same as the matrix found.
Confirmed visually on the saved waterfalls (e.g. combo 2 attempt 0: a
clear, bright burst at 918.5MHz around t=2.3s) -- though note the
printed `SDR peak@bin` number understates a real hit for a LoRa chirp
specifically, since a chirp continuously sweeps frequency across the
whole channel and only a fraction of its power sits at one exact
instant/bin; the waterfall PNG is the real evidence, the printed number
is just a quick directional glance.

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

`capture_spectrum.py`, `compare_spectrum.py`, `sync_capture.py`, and
`sync_cad_capture.py` are the first slices.
