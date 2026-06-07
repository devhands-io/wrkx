#!/usr/bin/env bash
# tests/e2e/memcached_counters.sh — memcached incr/decr E2E test (t064)
#
# Starts a dummy Python memcached text-protocol server and runs wrkx with a
# set → incr → decr counter workload.  A second run exercises NOT_FOUND
# handling by issuing INCR on a key that was never SET.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
MC_SERVER="$SCRIPT_DIR/memcached_mock_server.py"
LUA_COUNTERS="$ROOT_DIR/scripts/memcached_counters.lua"
PORT=19124

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make EXTENSIONS=memcached')" >&2
    exit 0
fi

probe=$("$WRK" -t1 -c1 -d1s -R1 "memcached://localhost:9" 2>&1 || true)
if echo "$probe" | grep -q 'no extension provides'; then
    echo "SKIP: memcached extension not built — rebuild with EXTENSIONS=memcached" >&2
    exit 0
fi

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "${TMPSCRIPT:-}"
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

# ---- low-rate counter run -----------------------------------------------
# -R20 over 3 s: expect ≥19 Requests/sec (within 5%).
output=$("$WRK" -t1 -c5 -d3s -R20 -s "$LUA_COUNTERS" "memcached://localhost:$PORT")
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
echo "PASS: low-rate counter run — Requests/sec=${rps}"

# ---- NOT_FOUND graceful-handling run ------------------------------------
# INCR on a key that was never SET; the server returns NOT_FOUND\r\n.
# wrkx must not crash and must still report throughput stats.
TMPSCRIPT=$(mktemp /tmp/mc_notfound_XXXXXX.lua)
cat > "$TMPSCRIPT" <<'EOF'
function request()
    return memcached.incr("key_that_was_never_set")
end
EOF

output_nf=$("$WRK" -t1 -c1 -d2s -R5 -s "$TMPSCRIPT" "memcached://localhost:$PORT" || true)
echo "$output_nf"

if ! echo "$output_nf" | grep -q "Requests/sec"; then
    echo "FAIL: NOT_FOUND run — 'Requests/sec' not found in output (wrkx crashed?)" >&2
    exit 1
fi
echo "PASS: NOT_FOUND run — wrkx handled NOT_FOUND replies gracefully"

# ---- higher-rate counter run --------------------------------------------
# -R200 over 3 s: expect ≥190 Requests/sec (within 5%).
output=$("$WRK" -t1 -c20 -d3s -R200 -s "$LUA_COUNTERS" "memcached://localhost:$PORT")
echo "$output"

rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 190 ]]; then
    echo "FAIL: Requests/sec ($rps) < 190 (expected ≥190 at -R200)" >&2
    exit 1
fi
echo "PASS: higher-rate counter run — Requests/sec=${rps}"

echo "PASS: memcached_counters"
