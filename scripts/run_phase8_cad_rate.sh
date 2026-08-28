#!/usr/bin/env bash
# Launch the strict Phase 8 CAD symbol-rate matrix in the background.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM0}"
HELTEC_PORT="${HELTEC_PORT:-/dev/ttyACM1}"
QUIET_CYCLES="${QUIET_CYCLES:-20}"
PULSE_CYCLES="${PULSE_CYCLES:-20}"
SYMBOLS="${SYMBOLS:-1,2,4,8,16}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-7200}"
OBSERVE_ONLY="${OBSERVE_ONLY:-0}"

if [[ ! -x "$PYTHON_BIN" || ! -e "$CARDPUTER_PORT" || ! -e "$HELTEC_PORT" ]]; then
    printf 'error: missing Python/Cardputer/Heltec; check host serial devices\n' >&2
    exit 1
fi

umask 077
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="$REPO_DIR/hardware-results/private/phase8"
mkdir -p "$LOG_DIR"
SERIAL_LOG="$LOG_DIR/phase8-cad-rate-${RUN_ID}.serial.log"
CONSOLE_LOG="$LOG_DIR/phase8-cad-rate-${RUN_ID}.console.log"
RESULTS_LOG="$LOG_DIR/phase8-cad-rate-${RUN_ID}.results.jsonl"
PID_FILE="$LOG_DIR/phase8-cad-rate-${RUN_ID}.pid"

CMD=(
    timeout "${MAX_RUNTIME_SECONDS}s"
    "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase8_cad_rate_bench.py"
    --cardputer-port "$CARDPUTER_PORT" --heltec-port "$HELTEC_PORT"
    --quiet-cycles "$QUIET_CYCLES" --pulse-cycles "$PULSE_CYCLES"
    --candidate LONG_MODERATE --symbols "$SYMBOLS" --log "$SERIAL_LOG" --results "$RESULTS_LOG"
)
if [[ "$OBSERVE_ONLY" == "1" ]]; then
    CMD+=(--observe-only)
fi

nohup "${CMD[@]}" > "$CONSOLE_LOG" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$PID_FILE"
printf 'CAD rate matrix launched. PID: %s\nResults: %s\nConsole: %s\n' "$(cat "$PID_FILE")" "$RESULTS_LOG" "$CONSOLE_LOG"
