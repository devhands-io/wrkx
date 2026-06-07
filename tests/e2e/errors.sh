#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
PORT=18083

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

python3 "$SERVER" "$PORT" flaky &
SERVER_PID=$!

# Wait for the server to be ready
for i in $(seq 1 20); do
    if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# Run wrkx; strip \r from progress-bar overwrite sequences
set +e
"$WRK" -t1 -c10 -d5s -R50 "http://localhost:$PORT/" 2>/dev/null | tr -d '\r' > "$TMP"
WRK_RC=${PIPESTATUS[0]}
set -e

if [[ "$WRK_RC" -ne 0 ]]; then
    echo "FAIL: wrkx exited with code $WRK_RC"
    cat "$TMP"
    exit 1
fi

# Assert the non-2xx line is present
if ! grep -q "Non-2xx/3xx:" "$TMP"; then
    echo "FAIL: 'Non-2xx/3xx:' not found in output (expected ~10% of ~250 requests)"
    cat "$TMP"
    exit 1
fi

# Extract the count and assert > 0
err_count=$(grep "Non-2xx/3xx:" "$TMP" | head -1 | awk '{print $NF}')
if [[ -z "$err_count" || "$err_count" -eq 0 ]]; then
    echo "FAIL: error count is 0 or missing (got: '$err_count')"
    cat "$TMP"
    exit 1
fi

echo "PASS: errors test (Non-2xx/3xx: ${err_count})"
