#!/usr/bin/env bash
# Launch a deterministic fault scenario against a cardputer-adv-bench image.

set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM1}"
HELTEC_PORT="${HELTEC_PORT:-}"
POINT="${POINT:-CAD_WAIT}"
ACTION="${ACTION:-FAIL}"
CYCLES="${CYCLES:-10}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-3600}"

if [[ ! -x "$PYTHON_BIN" || ! -e "$CARDPUTER_PORT" ]]; then
    printf 'error: set PYTHON_BIN and CARDPUTER_PORT after checking host devices\n' >&2
    exit 1
fi
if [[ -n "$HELTEC_PORT" && ! -e "$HELTEC_PORT" ]]; then
    printf 'error: HELTEC_PORT does not exist: %s\n' "$HELTEC_PORT" >&2
    exit 1
fi
umask 077
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="$REPO_DIR/hardware-results/private/phase8"
mkdir -p "$LOG_DIR"
SERIAL_LOG="$LOG_DIR/phase8-fault-${POINT,,}-${ACTION,,}-${CYCLES}-${RUN_ID}.serial.log"
CONSOLE_LOG="$LOG_DIR/phase8-fault-${POINT,,}-${ACTION,,}-${CYCLES}-${RUN_ID}.console.log"
RESULTS_LOG="$LOG_DIR/phase8-fault-${POINT,,}-${ACTION,,}-${CYCLES}-${RUN_ID}.results.jsonl"
printf 'point=%s action=%s cycles=%s cardputer_port=%s\n' "$POINT" "$ACTION" "$CYCLES" "$CARDPUTER_PORT" > "$CONSOLE_LOG"
CMD=(timeout "${MAX_RUNTIME_SECONDS}s" "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase8_fault_bench.py"
    --cardputer-port "$CARDPUTER_PORT" --log "$SERIAL_LOG" --results "$RESULTS_LOG"
    --point "$POINT" --action "$ACTION" --cycles "$CYCLES")
if [[ -n "$HELTEC_PORT" ]]; then
    CMD+=(--heltec-port "$HELTEC_PORT")
fi
nohup "${CMD[@]}" >> "$CONSOLE_LOG" 2>&1 < /dev/null &
PID=$!
printf '%s\n' "$PID" > "$LOG_DIR/phase8-fault-${RUN_ID}.pid"
printf 'Phase 8 fault harness launched (PID %s).\nConsole: %s\nSerial: %s\nResults: %s\n' "$PID" "$CONSOLE_LOG" "$SERIAL_LOG" "$RESULTS_LOG"
