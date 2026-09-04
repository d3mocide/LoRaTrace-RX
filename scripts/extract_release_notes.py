#!/usr/bin/env python3
"""Extracts one version's section from docs/RELEASE_NOTES.md.

release.yml uses this to publish curated, operator-facing notes as a
release body instead of GitHub's auto-generated commit list. Exits non-zero
with a ::error:: annotation when the section is missing, so a tag cannot
ship without notes -- the same "make it a build failure, not a review-time
catch" reasoning as the existing version.h/tag check.

Usage: extract_release_notes.py v1.0.6 [--notes docs/RELEASE_NOTES.md]
       extract_release_notes.py v1.0.6 --check      # verify only, no output
"""
import argparse
import pathlib
import re
import sys


def extract(text, tag):
    """Returns the body under `## <tag>` up to the next `## ` heading."""
    version = tag[1:] if tag.startswith("v") else tag
    # Accept "## v1.0.6" or "## 1.0.6"; tolerate trailing text after the
    # version (e.g. a date) so the heading style can grow later.
    pattern = re.compile(
        rf"^##\s+v?{re.escape(version)}\s*$.*?(?=^##\s|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(text)
    if match is None:
        return None
    section = match.group(0)
    # Drop the heading line itself; GitHub shows the tag as the title.
    body = section.split("\n", 1)[1] if "\n" in section else ""
    return body.strip("\n").rstrip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--notes", default="docs/RELEASE_NOTES.md")
    ap.add_argument("--check", action="store_true",
                    help="validate only; print nothing on success")
    args = ap.parse_args()

    path = pathlib.Path(args.notes)
    if not path.is_file():
        print(f"::error::{path} not found", file=sys.stderr)
        return 1

    body = extract(path.read_text(encoding="utf-8"), args.tag)
    if body is None:
        print(
            f"::error::No release-notes section for {args.tag} in {path}. "
            f"Add a '## {args.tag}' section describing what changed for someone "
            f"using the device, then re-run. See the header of that file for "
            f"what belongs there versus in CHANGELOG.md and src/version.h.",
            file=sys.stderr,
        )
        return 1
    if not body.strip():
        print(f"::error::The {args.tag} section in {path} is empty.", file=sys.stderr)
        return 1

    if not args.check:
        sys.stdout.write(body + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
