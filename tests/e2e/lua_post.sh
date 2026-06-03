#!/usr/bin/env bash
# tests/e2e/lua_post.sh
#
# Verifies that wrkx correctly loads and executes scripts/post.lua, which
# exercises POST body and Content-Type header injection via wrk.method,
# wrk.body, and wrk.headers.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
LUA_SCRIPT="$ROOT_DIR/scripts/post.lua"
PORT=18085

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK" >&2
    exit 0
fi

TMP=$(mktemp)
SERVER_PID=""
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$TMP"
}
trap cleanup EXIT

python3 "$SERVER" "$PORT" instant &
SERVER_PID=$!

for i in $(seq 1 20); do
    if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

set +e
"$WRK" -t1 -c5 -d3s -R20 \
    -s "$LUA_SCRIPT" \
    "http://localhost:$PORT/" 2>/dev/null | tr -d '\r' > "$TMP"
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

echo "PASS: lua_post test (POST script loaded and ran successfully)"
