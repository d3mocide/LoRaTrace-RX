#!/usr/bin/env bash
# One-time venv setup for this bench tool. System packages (rtl-sdr-blog,
# numpy/scipy/matplotlib) still need installing separately -- see README.md's
# "One-time setup" section for the pacman/yay commands, which need sudo this
# script deliberately doesn't run.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

python3 -m venv --system-site-packages "$VENV_DIR"
"$VENV_DIR/bin/python" -m pip install --ignore-installed pyrtlsdr
# pyserial: sync_capture.py drives the Cardputer/Heltec over the same
# scripts/bench_harness.py transport the rest of the project's bench
# scripts use, alongside the SDR capture.
"$VENV_DIR/bin/python" -m pip install pyserial

# PyPI's pyrtlsdr (as of 0.5.0) binds several dithering/GPIO functions that
# neither stock rtl-sdr (2.0.2) nor rtl-sdr-blog (1.4.0) export as of
# 2026-09 -- an unguarded `f = librtlsdr.rtlsdr_set_dithering`-style
# attribute access at import time, which raises AttributeError and makes
# the whole binding unusable even though this bench tool never calls any
# of those functions. Wrap that block in the same try/except AttributeError
# pattern the file's own author already uses for
# rtlsdr_set_and_get_tuner_bandwidth just above it, rather than patching
# individual symbols one crash at a time.
LIBRTLSDR_PY="$VENV_DIR/lib/python3.14/site-packages/rtlsdr/librtlsdr.py"
if [[ ! -f "$LIBRTLSDR_PY" ]]; then
    # Python minor version may differ from the one this script was written
    # against -- find it rather than hardcode.
    LIBRTLSDR_PY="$(find "$VENV_DIR/lib" -path '*/site-packages/rtlsdr/librtlsdr.py' | head -1)"
fi

python3 - "$LIBRTLSDR_PY" <<'PATCH'
import re
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()

if "rtlsdr_set_dithering" not in text:
    sys.exit(f"error: rtlsdr_set_dithering not found in {path} -- pyrtlsdr's "
             "bindings may have changed; check whether this patch still applies")
if "# LoRaTrace bench note" in text:
    print("already patched, skipping")
    sys.exit(0)

start_marker = "# RTLSDR_API int rtlsdr_set_dithering"
end_marker = "# RTLSDR_API int rtlsdr_set_gpio_status(rtlsdr_dev_t *dev, int *status )\nf = librtlsdr.rtlsdr_set_gpio_status\nf.restype, f.argtypes = c_int, [p_rtlsdr_dev, POINTER(c_int)]"

start = text.index(start_marker)
end = text.index(end_marker) + len(end_marker)
block = text[start:end]
indented = "\n".join(("    " + line if line.strip() else line) for line in block.splitlines())

replacement = (
    "# LoRaTrace bench note: dithering/GPIO control were added to pyrtlsdr's\n"
    "# bindings ahead of any librtlsdr release/fork that actually exports them\n"
    "# (neither stock rtl-sdr 2.0.2 nor rtl-sdr-blog 1.4.0 have these symbols as\n"
    "# of 2026-09) -- guarded the same way this file's own author already\n"
    "# guards rtlsdr_set_and_get_tuner_bandwidth below, rather than hard-failing\n"
    "# on an import this bench tool doesn't use any of these for.\n"
    "try:\n"
    f"{indented}\n"
    "except AttributeError:\n"
    "    pass"
)

text = text[:start] + replacement + text[end:]
open(path, "w", encoding="utf-8").write(text)
print(f"patched {path}")
PATCH

"$VENV_DIR/bin/python" -c "from rtlsdr import RtlSdr; print('pyrtlsdr import OK')"
echo "setup complete: $VENV_DIR"
