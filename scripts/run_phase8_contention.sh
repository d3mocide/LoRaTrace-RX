#!/usr/bin/env bash
# Launch Probe + high-rate STATUS contention against Cardputer and Heltec.

set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM1}"
HELTEC_PORT="${HELTEC_PORT:-/dev/ttyACM0}"
CYCLES="${CYCLES:-100}"
POLL_INTERVAL_MS="${POLL_INTERVAL_MS:-25}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-21600}"

for required in "$PYTHON_BIN" "$CARDPUTER_PORT" "$HELTEC_PORT"; do
    if [[ ! -e "$required" && "$required" != "$PYTHON_BIN" ]] || [[ "$required" == "$PYTHON_BIN" && ! -x "$required" ]]; then
        printf 'error: missing %s; check host serial devices and PYTHON_BIN\n' "$required" >&2
        exit 1
    fi
done
umask 077
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="$REPO_DIR/hardware-results/private/phase8"
mkdir -p "$LOG_DIR"
SERIAL_LOG="$LOG_DIR/phase8-contention-${CYCLES}-${RUN_ID}.serial.log"
CONSOLE_LOG="$LOG_DIR/phase8-contention-${CYCLES}-${RUN_ID}.console.log"
RESULTS_LOG="$LOG_DIR/phase8-contention-${CYCLES}-${RUN_ID}.results.jsonl"
printf 'cardputer_port=%s heltec_port=%s cycles=%s poll_interval_ms=%s\n' "$CARDPUTER_PORT" "$HELTEC_PORT" "$CYCLES" "$POLL_INTERVAL_MS" > "$CONSOLE_LOG"
nohup timeout "${MAX_RUNTIME_SECONDS}s" "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase8_contention_bench.py" \
    --cardputer-port "$CARDPUTER_PORT" --heltec-port "$HELTEC_PORT" --log "$SERIAL_LOG" \
    --results "$RESULTS_LOG" --cycles "$CYCLES" --poll-interval-ms "$POLL_INTERVAL_MS" >> "$CONSOLE_LOG" 2>&1 < /dev/null &
PID=$!
printf '%s\n' "$PID" > "$LOG_DIR/phase8-contention-${RUN_ID}.pid"
printf 'Phase 8 contention harness launched (PID %s).\nConsole: %s\nSerial: %s\nResults: %s\n' "$PID" "$CONSOLE_LOG" "$SERIAL_LOG" "$RESULTS_LOG"
