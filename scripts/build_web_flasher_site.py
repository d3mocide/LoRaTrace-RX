#!/usr/bin/env python3
"""Assembles the GitHub Pages web-flasher site from web-flasher/index.html
plus whatever release binaries CI has already downloaded to disk.

Binaries are copied into the site itself (not referenced by GitHub release
URL) because GitHub's release-asset redirect (release-assets.githubusercontent.com)
sends no Access-Control-Allow-Origin header — a browser fetch() from the
Pages origin would be blocked by CORS. Same-origin files sidestep that
entirely. See .github/workflows/pages.yml for how STABLE_DIR/DEV_DIR get
populated (gh release download, which is a server-side CI step and never
hits the browser CORS restriction).

Env vars (all required except STABLE_DIR/STABLE_VERSION, which are absent
when no stable release has been published yet):
  OUT_DIR         output directory for the assembled site
  TEMPLATE        path to web-flasher/index.html
  REPO_URL        e.g. https://github.com/d3mocide/LoRaTrace-RX
  BUILD_TIMESTAMP human-readable UTC timestamp for the footer
  DEV_DIR         directory holding the downloaded dev-latest release assets
  STABLE_DIR      directory holding the downloaded stable release assets (optional)
  STABLE_VERSION  tag name of the stable release, e.g. v1.0.0 (optional)
"""
import json
import os
import re
import shutil
import sys

# offset, source-asset suffix, dest filename in the assembled site.
# Offsets are ESP32-S3 flash layout, confirmed against a real `pio run -t
# upload -v` (esp32s3-devkitc-1 board, default_8MB.csv partitions) —
# see the pages.yml comment for how these were derived, not guessed.
PARTS = [
    (0, "bootloader.bin", "bootloader.bin"),
    (32768, "partitions.bin", "partitions.bin"),
    (57344, "boot_app0.bin", "boot_app0.bin"),
    (65536, None, "firmware.bin"),  # None: matched by exclusion below, see copy_track()
]


def copy_track(src_dir, dest_dir):
    """Copies one release's downloaded assets into dest_dir under fixed
    names, matching by suffix since the source filenames carry the
    version/tag (LoRaTraceRX-v1.0.0-bootloader.bin, LoRaTraceRX-dev.bin, ...).
    """
    os.makedirs(dest_dir, exist_ok=True)
    files = os.listdir(src_dir)
    for _, suffix, dest_name in PARTS:
        if suffix is not None:
            match = next((f for f in files if f.endswith(f"-{suffix}")), None)
        else:
            # The plain app binary has no "-firmware.bin" suffix — it's
            # whatever's left after the three named parts are excluded.
            named_suffixes = tuple(f"-{s}" for _, s, _ in PARTS if s is not None)
            match = next((f for f in files if not f.endswith(named_suffixes)), None)
        if match is None:
            print(f"::error::no asset matching '*-{suffix}' found in {src_dir} (have: {files})", file=sys.stderr)
            sys.exit(1)
        shutil.copyfile(os.path.join(src_dir, match), os.path.join(dest_dir, dest_name))


def write_manifest(out_path, version, bin_subdir):
    manifest = {
        "name": "LoRaTrace RX",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [
                    {"path": f"./bin/{bin_subdir}/{dest_name}", "offset": offset}
                    for offset, _, dest_name in PARTS
                ],
            }
        ],
    }
    with open(out_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


def strip_block(html, start_marker, end_marker, keep):
    pattern = re.compile(
        re.escape(f"<!-- {start_marker} -->") + r".*?" + re.escape(f"<!-- {end_marker} -->"),
        re.DOTALL,
    )
    if keep:
        # Drop just the marker comments, keep the content between them.
        html = html.replace(f"<!-- {start_marker} -->\n", "").replace(f"<!-- {end_marker} -->\n", "")
        return html
    return pattern.sub("", html)


def main():
    out_dir = os.environ["OUT_DIR"]
    template_path = os.environ["TEMPLATE"]
    repo_url = os.environ["REPO_URL"]
    build_timestamp = os.environ["BUILD_TIMESTAMP"]
    dev_dir = os.environ["DEV_DIR"]
    stable_dir = os.environ.get("STABLE_DIR", "")
    stable_version = os.environ.get("STABLE_VERSION", "")

    has_stable = bool(stable_dir and stable_version and os.path.isdir(stable_dir))

    os.makedirs(out_dir, exist_ok=True)

    copy_track(dev_dir, os.path.join(out_dir, "bin", "dev"))
    write_manifest(os.path.join(out_dir, "manifest-dev.json"), "dev", "dev")

    if has_stable:
        copy_track(stable_dir, os.path.join(out_dir, "bin", "stable"))
        write_manifest(os.path.join(out_dir, "manifest-stable.json"), stable_version, "stable")

    with open(template_path) as f:
        html = f.read()

    html = strip_block(html, "STABLE_START", "STABLE_END", keep=has_stable)
    html = strip_block(html, "STABLE_NONE_START", "STABLE_NONE_END", keep=not has_stable)

    html = html.replace("__STABLE_VERSION__", stable_version)
    html = html.replace("__REPO_URL__", repo_url)
    html = html.replace("__BUILD_TIMESTAMP__", build_timestamp)

    if has_stable:
        html = html.replace('id="stable-bin-link" href="#"', 'id="stable-bin-link" href="./bin/stable/firmware.bin"')
    html = html.replace('id="dev-bin-link" href="#"', 'id="dev-bin-link" href="./bin/dev/firmware.bin"')

    with open(os.path.join(out_dir, "index.html"), "w") as f:
        f.write(html)

    print(f"Assembled site in {out_dir} (stable={'yes:' + stable_version if has_stable else 'no'})")


if __name__ == "__main__":
    main()
