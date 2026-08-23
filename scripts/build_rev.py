# LoRaTrace RX — inject the git revision into the build.
#
# Why this exists: `src/version.h` carries the deliberate semantic version
# (MAJOR.MINOR = build-order phase, per ROADMAP.md Versioning). That number
# should stay a *statement* a human makes when a phase is actually reached —
# auto-incrementing it would make it meaningless.
#
# What genuinely can be automated is provenance: which commit is this build?
# Rolling `dev-latest` builds get flashed constantly during hardware testing,
# and "v0.2.0" alone cannot distinguish two builds an hour apart. That's the
# gap this closes, and it's the one that actually costs debugging time — a
# hardware report against an ambiguous binary is close to unusable.
#
# Python rather than a shell command in build_flags because this project is
# built on Windows (see PROGRESS.md hardware sessions); `$(git ...)` in
# platformio.ini does not work under cmd.exe. PlatformIO always ships Python,
# so this runs identically on Windows, Linux and CI.
#
# Degrades gracefully: a source drop with no .git, or no git binary, yields
# "nogit" rather than failing the build.

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

import subprocess


def _git(*args):
    try:
        out = subprocess.check_output(
            ["git", *args], stderr=subprocess.DEVNULL, timeout=10
        )
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return ""


rev = _git("rev-parse", "--short", "HEAD") or "nogit"

# A "-dirty" suffix matters more than it looks: most hardware testing happens
# from a working tree with uncommitted edits, and a bug report citing a clean
# SHA that doesn't reproduce is a genuine time sink.
if _git("status", "--porcelain"):
    rev += "-dirty"

env.Append(CPPDEFINES=[("FIRMWARE_BUILD_REV", env.StringifyMacro(rev))])  # noqa: F821
print("LoRaTrace build revision: %s" % rev)
