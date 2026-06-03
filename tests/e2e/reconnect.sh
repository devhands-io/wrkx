#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
PORT=18082

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

SERVER_PID=""
WRK_PID=""
cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "$WRK_PID"    ]] && kill "$WRK_PID"    2>/dev/null || true
}
trap cleanup EXIT

python3 "$SERVER" "$PORT" drop &
SERVER_PID=$!

# Wait for the server to be ready
for i in $(seq 1 20); do
    if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# Run wrkx in the background; suppress output
"$WRK" -t1 -c5 -d8s -R10 "http://localhost:$PORT/" > /dev/null 2>&1 &
WRK_PID=$!

# Wait to midpoint, then sample CPU
sleep 4

# A crash is not a pass
if ! kill -0 "$WRK_PID" 2>/dev/null; then
    echo "FAIL: wrkx exited before the CPU sample could be taken"
    exit 1
fi

CPU=$(ps -o %cpu= -p "$WRK_PID" 2>/dev/null | tr -d ' ')
if [[ -z "$CPU" ]]; then
    echo "FAIL: could not read CPU% for PID $WRK_PID"
    exit 1
fi

TOO_HIGH=$(echo "$CPU >= 90" | bc)
if [[ "$TOO_HIGH" -eq 1 ]]; then
    echo "FAIL: CPU ${CPU}% >= 90% — reconnect bug may be present"
    exit 1
fi

# Wait for wrkx to finish cleanly
set +e
wait "$WRK_PID"
WRK_RC=$?
set -e

if [[ "$WRK_RC" -ne 0 ]]; then
    echo "FAIL: wrkx exited with code $WRK_RC"
    exit 1
fi

echo "PASS: reconnect test (CPU at midpoint: ${CPU}%)"
