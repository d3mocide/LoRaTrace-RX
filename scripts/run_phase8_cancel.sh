#!/usr/bin/env bash
# Launch the Phase 8 cancellation scenario independently of the terminal.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM2}"
CYCLES="${CYCLES:-10}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-3600}"

if [[ ! -x "$PYTHON_BIN" ]]; then
    printf 'error: Python executable not found: %s\n' "$PYTHON_BIN" >&2
    exit 1
fi
if [[ ! -e "$CARDPUTER_PORT" ]]; then
    printf 'error: serial device not found: %s\n' "$CARDPUTER_PORT" >&2
    exit 1
fi
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
SERIAL_LOG="$LOG_DIR/phase8-cancel-${CYCLES}-${RUN_ID}.serial.log"
CONSOLE_LOG="$LOG_DIR/phase8-cancel-${CYCLES}-${RUN_ID}.console.log"
RESULTS_LOG="$LOG_DIR/phase8-cancel-${CYCLES}-${RUN_ID}.results.jsonl"
PID_FILE="$LOG_DIR/phase8-cancel-${CYCLES}-${RUN_ID}.pid"

{
    printf 'started_utc=%s\n' "$RUN_ID"
    printf 'cardputer_port=%s\n' "$CARDPUTER_PORT"
    printf 'cycles=%s\n' "$CYCLES"
    printf 'serial_log=%s\n' "$SERIAL_LOG"
    printf 'results_log=%s\n\n' "$RESULTS_LOG"
} > "$CONSOLE_LOG"

nohup timeout "${MAX_RUNTIME_SECONDS}s" \
    "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase8_cancel_bench.py" \
    --cardputer-port "$CARDPUTER_PORT" \
    --log "$SERIAL_LOG" \
    --results "$RESULTS_LOG" \
    --cycles "$CYCLES" \
    >> "$CONSOLE_LOG" 2>&1 < /dev/null &
PID=$!
printf '%s\n' "$PID" > "$PID_FILE"

printf 'Phase 8 cancellation harness launched.\n'
printf 'PID:          %s\n' "$PID"
printf 'Console log:  %s\n' "$CONSOLE_LOG"
printf 'Serial log:   %s\n' "$SERIAL_LOG"
printf 'Results log:  %s\n' "$RESULTS_LOG"
printf 'PID file:     %s\n' "$PID_FILE"
printf 'Follow output: tail -f %q\n' "$CONSOLE_LOG"
