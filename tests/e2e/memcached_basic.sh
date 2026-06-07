#!/usr/bin/env bash
# tests/e2e/memcached_basic.sh — memcached GET E2E test (ADR 0005, P4-1, t062)
#
# Starts a dummy Python memcached text-protocol server and runs wrkx against
# memcached://localhost:PORT.  Asserts Requests/sec is within 5% of the -R
# target.  No real memcached installation required.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
MC_SERVER="$SCRIPT_DIR/memcached_mock_server.py"
LUA_SCRIPT="$ROOT_DIR/scripts/memcached_get.lua"
PORT=19122

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make EXTENSIONS=memcached')" >&2
    exit 0
fi

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

python3 "$MC_SERVER" "$PORT" &
SERVER_PID=$!

# Wait up to 2 s for the server to accept connections.
for i in $(seq 1 20); do
    if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# ---- low-rate run -------------------------------------------------------
# -R20 over 3 s: expect ≥19 Requests/sec (within 5%).
output=$("$WRK" -t1 -c5 -d3s -R20 -s "$LUA_SCRIPT" "memcached://localhost:$PORT")
echo "$output"

if ! echo "$output" | grep -q "Requests/sec"; then
    echo "FAIL: 'Requests/sec' not found in output" >&2
    exit 1
fi
if ! echo "$output" | grep -q "Transfer/sec"; then
    echo "FAIL: 'Transfer/sec' not found in output" >&2
    exit 1
fi

rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 19 ]]; then
    echo "FAIL: Requests/sec ($rps) < 19 (expected ≥19 at -R20)" >&2
    exit 1
fi
echo "PASS: low-rate run — Requests/sec=${rps}"

# ---- higher-rate run ----------------------------------------------------
# -R200 over 3 s: expect ≥190 Requests/sec (within 5%).
output=$("$WRK" -t1 -c20 -d3s -R200 -s "$LUA_SCRIPT" "memcached://localhost:$PORT")
echo "$output"

rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 190 ]]; then
    echo "FAIL: Requests/sec ($rps) < 190 (expected ≥190 at -R200)" >&2
    exit 1
fi
echo "PASS: higher-rate run — Requests/sec=${rps}"

echo "PASS: memcached_basic"
