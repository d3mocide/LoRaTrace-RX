#!/usr/bin/env bash
# Run every named bench fault point with both CANCEL and FAIL actions from one
# stable Cardputer/Heltec boot. Reopening native USB for each case would reset
# the Cardputer and contaminate the matrix with boot/SD watchdog effects.

set -u
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM1}"
HELTEC_PORT="${HELTEC_PORT:-/dev/ttyACM0}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-3600}"
LOG_DIR="$REPO_DIR/hardware-results/private/phase8"

if [[ ! -x "$PYTHON_BIN" || ! -e "$CARDPUTER_PORT" || ! -e "$HELTEC_PORT" ]]; then
    printf 'error: missing Python/Cardputer/Heltec; check host serial devices\n' >&2
    exit 1
fi
umask 077
mkdir -p "$LOG_DIR"
run_id="$(date -u +%Y%m%dT%H%M%SZ)"
serial_log="$LOG_DIR/phase8-fault-matrix-${run_id}.serial.log"
console_log="$LOG_DIR/phase8-fault-matrix-${run_id}.console.log"
results_log="$LOG_DIR/phase8-fault-matrix-${run_id}.results.jsonl"
printf 'started_utc=%s cardputer_port=%s heltec_port=%s\n' "$run_id" "$CARDPUTER_PORT" "$HELTEC_PORT" > "$console_log"
timeout "${MAX_RUNTIME_SECONDS}s" "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase8_fault_matrix_bench.py" \
    --cardputer-port "$CARDPUTER_PORT" --heltec-port "$HELTEC_PORT" \
    --log "$serial_log" --results "$results_log" >> "$console_log" 2>&1
status=$?
printf 'exit_status=%s\n' "$status" >> "$console_log"
printf 'Matrix console: %s\nMatrix serial:  %s\nMatrix results: %s\n' "$console_log" "$serial_log" "$results_log"
exit "$status"
