#!/usr/bin/env bash
# tests/e2e/quickjs_redis_mock.sh
#
# End-to-end smoke test for the QuickJS engine Redis path (ADR 0005, t075).
# Spins up the shared Redis mock server and drives wrkx with --engine=quickjs
# and a redis:// URL so the Redis protocol vtable is selected.
#
# SKIP conditions:
#   - wrkx binary not found (build first)
#   - wrkx built without QuickJS (./configure --with-quickjs)
#   - mock server unavailable
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/redis_mock_server.py"
JS_SCRIPT="$ROOT_DIR/scripts/redis_get_set.js"
PORT=18091

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

if [[ ! -f "$JS_SCRIPT" ]]; then
    echo "SKIP: $JS_SCRIPT not found" >&2
    exit 0
fi

# Check QuickJS support.
if "$WRK" --engine=quickjs -t1 -c1 -d1s -R1 "http://127.0.0.1:1" 2>&1 \
        | grep -q "built without QuickJS"; then
    echo "SKIP: wrkx built without QuickJS (./configure --with-quickjs)" >&2
    exit 0
fi

if [[ ! -f "$SERVER" ]]; then
    echo "SKIP: Redis mock server not found at $SERVER" >&2
    exit 0
fi

TMP=$(mktemp)
SERVER_PID=""
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$TMP"
}
trap cleanup EXIT

python3 "$SERVER" "$PORT" &
SERVER_PID=$!

# Wait for the server to accept connections.
for i in $(seq 1 20); do
    if python3 -c \
        "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" \
        2>/dev/null; then
        break
    fi
    sleep 0.1
done

set +e
"$WRK" -t2 -c10 -d3s -R50 \
    --engine=quickjs \
    -s "$JS_SCRIPT" \
    "redis://127.0.0.1:$PORT/" 2>/dev/null | tr -d '\r' > "$TMP"
WRK_RC=${PIPESTATUS[0]}
set -e

if [[ "$WRK_RC" -ne 0 ]]; then
    echo "FAIL: wrkx exited with code $WRK_RC"
    cat "$TMP"
    exit 1
fi

if ! grep -q "Requests/sec" "$TMP"; then
    echo "FAIL: 'Requests/sec' not found in output"
    cat "$TMP"
    exit 1
fi

echo "PASS: quickjs_redis_mock test (QuickJS+Redis drove workload successfully)"
