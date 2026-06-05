#!/usr/bin/env bash
# tests/e2e/rate_close.sh
#
# Regression guard for the phantom-completion rate flood (t036).
#
# Against a "Connection: close" server (reconnect after every request) the
# open-model rate limiter (-R) must still hold the target throughput. Before
# the fix, http1_readable left s->complete set after reporting a response, so
# the level-triggered EOF that follows a close re-reported the SAME response in
# a tight loop. That double-counted completions and made throughput run ~10x
# over the -R target with multi-second latencies.
#
# This test asserts the measured Requests/sec stays within a sane band of the
# requested rate (not 10x over) when every response closes the connection.
#
# Uses the mock server's "close" mode (port 18098).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
PORT=18098
RATE=2000

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

TMP=$(mktemp)
SERVER_PID=""
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$TMP"
}
trap cleanup EXIT

python3 "$SERVER" "$PORT" close &
SERVER_PID=$!

for i in $(seq 1 20); do
    if python3 -c \
        "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" \
        2>/dev/null; then
        break
    fi
    sleep 0.1
done

# Short run: duration < calibration window is fine — the flood (when present)
# shows up immediately. Strip \r from the progress bar.
set +e
"$WRK" -t4 -c8 -d4s -R"$RATE" "http://localhost:$PORT/" 2>/dev/null \
    | tr -d '\r' > "$TMP"
WRK_RC=${PIPESTATUS[0]}
set -e

if [[ "$WRK_RC" -ne 0 ]]; then
    echo "FAIL: wrkx exited with code $WRK_RC"
    cat "$TMP"
    exit 1
fi

rps=$(grep "Requests/sec:" "$TMP" | head -1 | awk '{print $2}')
if [[ -z "$rps" ]]; then
    echo "FAIL: could not parse Requests/sec from output"
    cat "$TMP"
    exit 1
fi

# Integer-compare (drop the fractional part).
rps_int=${rps%.*}

# Upper bound: must not exceed ~1.5x the target. The pre-fix flood ran ~10x
# over (e.g. -R2000 -> ~20000+). A correctly paced run sits at or below the
# target (reconnect overhead usually keeps it slightly under).
UPPER=$(( RATE * 3 / 2 ))
if [[ "$rps_int" -gt "$UPPER" ]]; then
    echo "FAIL: Requests/sec ${rps} exceeds ${UPPER} (target ${RATE}) — rate flood regression"
    cat "$TMP"
    exit 1
fi

echo "PASS: rate_close test (Requests/sec ${rps} <= ${UPPER}, target ${RATE})"
