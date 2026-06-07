#!/usr/bin/env bash
# tests/e2e/memcached_robustness.sh — connection reuse and failure-mode tests
# (ADR 0005, Phase 4, t065)
#
# Tests three robustness scenarios:
#   1. Connection reuse: single connection, many requests.
#   2. Server-close recovery: server closes each connection after N commands;
#      wrkx must reconnect and continue serving requests.
#   3. Malformed reply: server sends garbage; wrkx must exit cleanly (no crash,
#      no hang).  All requests error — that is expected and correct.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
MC_SERVER="$SCRIPT_DIR/memcached_mock_server.py"
LUA_SCRIPT="$ROOT_DIR/scripts/memcached_get.lua"
PORT=19125

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make EXTENSIONS=memcached')" >&2
    exit 0
fi

probe=$("$WRK" -t1 -c1 -d1s -R1 "memcached://localhost:9" 2>&1 || true)
if echo "$probe" | grep -q 'no extension provides'; then
    echo "SKIP: memcached extension not built — rebuild with EXTENSIONS=memcached" >&2
    exit 0
fi

SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}
trap cleanup EXIT

wait_server() {
    for i in $(seq 1 20); do
        if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "FAIL: mock server did not start in time" >&2
    exit 1
}

start_server() {
    python3 "$MC_SERVER" "$PORT" "$@" &
    SERVER_PID=$!
    wait_server
}

stop_server() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

# ---- Test 1: connection reuse -----------------------------------------------
# -c1 (single connection), -R50 over 3 s: all requests must flow through the
# same TCP connection without reconnects tanking throughput.
echo "--- Test 1: connection reuse (single connection) ---"
start_server
output=$("$WRK" -t1 -c1 -d3s -R50 -s "$LUA_SCRIPT" "memcached://localhost:$PORT")
echo "$output"
rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 47 ]]; then
    echo "FAIL: Requests/sec ($rps) < 47 with single connection (expected ≥47 at -R50)" >&2
    exit 1
fi
echo "PASS: connection reuse — Requests/sec=${rps}"
stop_server

# ---- Test 2: server-close recovery ------------------------------------------
# Server closes each connection after 3 commands.  wrkx must detect the close
# (TRANSPORT_EOF → PROTO_ERROR), reconnect, and continue.  Total Requests/sec
# must remain nonzero despite the forced reconnects.
echo "--- Test 2: server-close recovery ---"
start_server --close-after 3
output=$("$WRK" -t1 -c5 -d3s -R20 -s "$LUA_SCRIPT" "memcached://localhost:$PORT")
echo "$output"
if ! echo "$output" | grep -q "Requests/sec"; then
    echo "FAIL: 'Requests/sec' not found in output" >&2
    exit 1
fi
rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 1 ]]; then
    echo "FAIL: Requests/sec ($rps) is 0 — reconnect recovery did not work" >&2
    exit 1
fi
echo "PASS: server-close recovery — Requests/sec=${rps}"
stop_server

# ---- Test 3: malformed reply handling ---------------------------------------
# Server sends garbage on every reply.  The codec returns MC_STATUS_ERROR →
# PROTO_ERROR → orchestrator reconnects.  wrkx must exit with status 0 (no
# crash, no hang).  Zero successful Requests/sec is expected and acceptable.
echo "--- Test 3: malformed reply handling ---"
start_server --bad-reply
if ! "$WRK" -t1 -c3 -d2s -R10 -s "$LUA_SCRIPT" "memcached://localhost:$PORT" >/dev/null 2>&1; then
    echo "FAIL: wrkx exited non-zero on malformed replies (expected clean exit)" >&2
    exit 1
fi
echo "PASS: malformed reply — wrkx exited cleanly"
stop_server

echo "PASS: memcached_robustness"
