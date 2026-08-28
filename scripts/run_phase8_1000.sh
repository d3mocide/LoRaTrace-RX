#!/usr/bin/env bash
# Launch the two-device Phase 8 bench independently of the calling terminal.
# Raw serial captures stay under hardware-results/private/ (git-ignored).

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM2}"
HELTEC_PORT="${HELTEC_PORT:-/dev/ttyACM0}"
CYCLES="${CYCLES:-1000}"
ARM_DELAY_MS="${ARM_DELAY_MS:-100}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-21600}"

if [[ ! -x "$PYTHON_BIN" ]]; then
    printf 'error: Python executable not found: %s\n' "$PYTHON_BIN" >&2
    exit 1
fi
if ! command -v timeout >/dev/null 2>&1; then
    printf 'error: timeout command is required\n' >&2
    exit 1
fi
for port in "$CARDPUTER_PORT" "$HELTEC_PORT"; do
    if [[ ! -e "$port" ]]; then
        printf 'error: serial device not found: %s\n' "$port" >&2
        printf '       set CARDPUTER_PORT/HELTEC_PORT after checking the host devices\n' >&2
        exit 1
    fi
done

case "$CYCLES" in
    ''|*[!0-9]*) printf 'error: CYCLES must be an integer (1..1000)\n' >&2; exit 1 ;;
esac
if (( CYCLES < 1 || CYCLES > 1000 )); then
    printf 'error: CYCLES must be in the range 1..1000\n' >&2
    exit 1
fi

umask 077
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="$REPO_DIR/hardware-results/private/phase8"
mkdir -p "$LOG_DIR"
SERIAL_LOG="$LOG_DIR/phase8-${CYCLES}-${RUN_ID}.serial.log"
CONSOLE_LOG="$LOG_DIR/phase8-${CYCLES}-${RUN_ID}.console.log"
RESULTS_LOG="$LOG_DIR/phase8-${CYCLES}-${RUN_ID}.results.jsonl"
PID_FILE="$LOG_DIR/phase8-${CYCLES}-${RUN_ID}.pid"

{
    printf 'started_utc=%s\n' "$RUN_ID"
    printf 'cardputer_port=%s\n' "$CARDPUTER_PORT"
    printf 'heltec_port=%s\n' "$HELTEC_PORT"
    printf 'cycles=%s\n' "$CYCLES"
    printf 'arm_delay_ms=%s\n' "$ARM_DELAY_MS"
    printf 'serial_log=%s\n' "$SERIAL_LOG"
    printf 'console_log=%s\n' "$CONSOLE_LOG"
    printf 'results_log=%s\n' "$RESULTS_LOG"
    printf '\n'
} > "$CONSOLE_LOG"

CMD=(
    timeout "${MAX_RUNTIME_SECONDS}s"
    "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase8_bench.py"
    --cardputer-port "$CARDPUTER_PORT"
    --heltec-port "$HELTEC_PORT"
    --log "$SERIAL_LOG"
    --results "$RESULTS_LOG"
    --cycles "$CYCLES"
    --arm-delay-ms "$ARM_DELAY_MS"
)

nohup "${CMD[@]}" >> "$CONSOLE_LOG" 2>&1 < /dev/null &
PID=$!
printf '%s\n' "$PID" > "$PID_FILE"

printf 'Phase 8 harness launched.\n'
printf 'PID:          %s\n' "$PID"
printf 'Serial log:   %s\n' "$SERIAL_LOG"
printf 'Console log:  %s\n' "$CONSOLE_LOG"
printf 'PID file:      %s\n' "$PID_FILE"
printf 'Follow output: tail -f %q\n' "$CONSOLE_LOG"
