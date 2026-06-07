#!/usr/bin/env bash
# tests/e2e/quickjs_http.sh
#
# End-to-end smoke test for the QuickJS engine HTTP path (ADR 0005, t073).
# Spins up the shared mock HTTP server and drives wrkx with --engine=quickjs.
#
# SKIP conditions:
#   - wrkx binary not found (build first)
#   - wrkx built without QuickJS (./configure --with-quickjs)
#   - mock server unavailable
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
JS_SCRIPT="$ROOT_DIR/scripts/http_basic.js"
PORT=18090

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

# Check QuickJS support via the --engine flag; a clean stderr message means
# the binary was built without it.
if "$WRK" --engine=quickjs -t1 -c1 -d1s -R1 "http://127.0.0.1:1" 2>&1 \
        | grep -q "built without QuickJS"; then
    echo "SKIP: wrkx built without QuickJS (./configure --with-quickjs)" >&2
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

# Use -t1 for t073: JSContext is not thread-safe without clone() (t074).
# Multi-thread isolation is verified once clone is wired in t074.
set +e
"$WRK" -t1 -c5 -d3s -R50 \
    --engine=quickjs \
    -s "$JS_SCRIPT" \
    "http://127.0.0.1:$PORT/" 2>/dev/null | tr -d '\r' > "$TMP"
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

echo "PASS: quickjs_http test (QuickJS engine drove HTTP workload successfully)"
