#!/usr/bin/env bash
# Audit A27: fuzz the parsers that eat over-the-air or on-card bytes, under
# AddressSanitizer and UndefinedBehaviorSanitizer.
#
# These are the parsers an attacker reaches by transmitting, and the formatters
# that write what they produce into a CSV row. `pio test -e native` covers what
# they do with input we thought of; this covers the rest.
#
#   scripts/fuzz.sh              # 30s per target
#   scripts/fuzz.sh 300          # 5 minutes per target
#   scripts/fuzz.sh 60 nmea      # one target
#
# A finding is a crash file under fuzz/artifacts/; re-run a single case with
#   ./fuzz/build/fuzz_<target> fuzz/artifacts/<file>
set -uo pipefail
cd "$(dirname "$0")/.."

SECS="${1:-30}"
ONLY="${2:-}"
BUILD=fuzz/build
CORPUS=fuzz/corpus
ART=fuzz/artifacts
mkdir -p "$BUILD" "$ART"

CXX="${CXX:-clang++}"
if ! command -v "$CXX" >/dev/null; then
    echo "need clang++ for -fsanitize=fuzzer (set CXX=)" >&2
    exit 1
fi

FLAGS=(-std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined
       -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra)

status=0
for src in fuzz/fuzz_*.cpp; do
    name="$(basename "$src" .cpp)"
    target="${name#fuzz_}"
    [ -n "$ONLY" ] && [ "$target" != "$ONLY" ] && continue

    echo "=== building $target ==="
    if ! "$CXX" "${FLAGS[@]}" "$src" -o "$BUILD/$name"; then
        echo "BUILD FAILED: $target" >&2
        status=1
        continue
    fi

    mkdir -p "$CORPUS/$target"
    echo "=== fuzzing $target for ${SECS}s ==="
    # -artifact_prefix keeps a reproducer next to the corpus it came from.
    if ! "$BUILD/$name" "$CORPUS/$target" \
            -max_total_time="$SECS" -max_len=1024 -rss_limit_mb=2048 \
            -artifact_prefix="$ART/${target}-" 2>&1 | tail -5; then
        echo "FINDING in $target — see $ART/${target}-*" >&2
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "all targets clean"
else
    echo "at least one target failed or found something" >&2
fi
exit "$status"
