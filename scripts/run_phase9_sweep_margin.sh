#!/usr/bin/env bash
# Launch the Phase 9 Sweep noise-floor margin calibration bench in the
# background. Requires the cardputer-adv-bench image already flashed.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/home/d3mo/.platformio/penv/bin/python}"
CARDPUTER_PORT="${CARDPUTER_PORT:-/dev/ttyACM1}"
HELTEC_PORT="${HELTEC_PORT:-/dev/ttyACM0}"
MARGINS="${MARGINS:-50,100,150,200,250,300}"
PULSE_INTERVAL_S="${PULSE_INTERVAL_S:-2.0}"
ARM_DELAY_MS="${ARM_DELAY_MS:-250}"
REPEATS="${REPEATS:-1}"
MAX_RUNTIME_SECONDS="${MAX_RUNTIME_SECONDS:-3600}"

if [[ ! -x "$PYTHON_BIN" || ! -e "$CARDPUTER_PORT" || ! -e "$HELTEC_PORT" ]]; then
    printf 'error: missing Python/Cardputer/Heltec; check host serial devices\n' >&2
    exit 1
fi

umask 077
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="$REPO_DIR/hardware-results/private/phase9"
mkdir -p "$LOG_DIR"
SERIAL_LOG="$LOG_DIR/phase9-sweep-margin-${RUN_ID}.serial.log"
CONSOLE_LOG="$LOG_DIR/phase9-sweep-margin-${RUN_ID}.console.log"
RESULTS_LOG="$LOG_DIR/phase9-sweep-margin-${RUN_ID}.results.jsonl"
PID_FILE="$LOG_DIR/phase9-sweep-margin-${RUN_ID}.pid"

CMD=(
    timeout "${MAX_RUNTIME_SECONDS}s"
    "$PYTHON_BIN" -u "$REPO_DIR/scripts/phase9_sweep_margin_bench.py"
    --cardputer-port "$CARDPUTER_PORT" --heltec-port "$HELTEC_PORT"
    --margins "$MARGINS" --pulse-interval-s "$PULSE_INTERVAL_S" --arm-delay-ms "$ARM_DELAY_MS"
    --repeats "$REPEATS" --log "$SERIAL_LOG" --results "$RESULTS_LOG"
)

nohup "${CMD[@]}" > "$CONSOLE_LOG" 2>&1 < /dev/null &
printf '%s\n' "$!" > "$PID_FILE"
printf 'Sweep margin matrix launched. PID: %s\nResults: %s\nConsole: %s\n' "$(cat "$PID_FILE")" "$RESULTS_LOG" "$CONSOLE_LOG"
