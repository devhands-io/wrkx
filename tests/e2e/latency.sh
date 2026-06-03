#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
PORT=18081

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

TMP=$(mktemp)
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$TMP"
}
trap cleanup EXIT

python3 "$SERVER" "$PORT" delay &
SERVER_PID=$!

# Wait for the server to be ready
for i in $(seq 1 20); do
    if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# Run wrkx; strip \r so progress-bar overwrite sequences don't confuse grep/awk
set +e
"$WRK" -t1 -c5 -d20s -R20 -L "http://localhost:$PORT/" 2>/dev/null | tr -d '\r' > "$TMP"
WRK_RC=${PIPESTATUS[0]}
set -e

if [[ "$WRK_RC" -ne 0 ]]; then
    echo "FAIL: wrkx exited with code $WRK_RC"
    cat "$TMP"
    exit 1
fi

# Assert latency distribution section is present
if ! grep -q "Latency Distribution (HdrHistogram" "$TMP"; then
    echo "FAIL: 'Latency Distribution (HdrHistogram' not found in output"
    cat "$TMP"
    exit 1
fi

# Parse 50th-percentile value and convert to microseconds
val_us=$(grep '50\.000%' "$TMP" | head -1 | awk '{
    v = $2
    u = substr(v, length(v) - 1)
    n = substr(v, 1, length(v) - 2)
    if (u == "ms") print int(n * 1000)
    else            print int(n)
}')

if [[ -z "$val_us" ]]; then
    echo "FAIL: could not parse 50th-percentile value from output"
    cat "$TMP"
    exit 1
fi

if [[ "$val_us" -lt 40000 || "$val_us" -gt 300000 ]]; then
    echo "FAIL: 50th-percentile ${val_us}us is outside [40000, 300000]us (40-300ms)"
    cat "$TMP"
    exit 1
fi

echo "PASS: latency test (50th percentile: ${val_us}us)"
