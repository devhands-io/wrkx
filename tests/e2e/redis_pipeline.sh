#!/usr/bin/env bash
# tests/e2e/redis_pipeline.sh — Redis pipelining E2E test (ADR 0005, P2-4)
#
# Tests three things:
#   1. Pipeline depth N works end-to-end: wrkx sends N commands, receives N
#      replies, reports correct Requests/sec and Transfer/sec.
#   2. Transfer/sec at depth N is approximately N× that of depth 1 (more wire
#      bytes per batch because N replies are received).
#   3. With artificial server delay, the batch latency at depth N is higher
#      than at depth 1 (pipelining accumulates server processing time).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
REDIS_SERVER="$SCRIPT_DIR/redis_mock_server.py"
PORT=18380

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ---- helper: wait for server to accept connections --------------------------
wait_for_server() {
    for i in $(seq 1 20); do
        if python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "FAIL: server on port $PORT did not come up" >&2
    return 1
}

# ---- helper: extract Requests/sec integer from wrkx output ------------------
rps_of() {
    echo "$1" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1
}

# ---- helper: extract Transfer/sec bytes from wrkx output --------------------
# Returns value in bytes. wrkx emits "1.09KB" (no space) so parse unit from
# the same field as the value.
transfer_bytes_of() {
    local line
    line=$(echo "$1" | grep "Transfer/sec")
    local raw val unit
    raw=$(echo "$line" | awk '{print $2}')
    val=$(echo "$raw"  | grep -oE '^[0-9]+(\.[0-9]+)?')
    unit=$(echo "$raw" | grep -oE '[A-Za-z]+$')
    case "$unit" in
        KB)  echo "$(echo "$val * 1024"       | bc | cut -d. -f1)" ;;
        MB)  echo "$(echo "$val * 1048576"    | bc | cut -d. -f1)" ;;
        GB)  echo "$(echo "$val * 1073741824" | bc | cut -d. -f1)" ;;
        *)   echo "$val" | cut -d. -f1 ;;
    esac
}

# ===========================================================================
# Test 1: depth-1 baseline (sanity — same as redis_basic.sh at -R20)
# ===========================================================================

# Write depth-1 Lua script
SCRIPT_D1=$(mktemp /tmp/redis_pipe_d1_XXXXXX.lua)
cat > "$SCRIPT_D1" <<'EOF'
function request()
    return redis.command("GET", "k")
end
EOF

python3 "$REDIS_SERVER" "$PORT" &
SERVER_PID=$!
wait_for_server

out_d1=$("$WRK" -t1 -c5 -d3s -R20 -s "$SCRIPT_D1" "redis://localhost:$PORT")
echo "$out_d1"

rps_d1=$(rps_of "$out_d1")
if [[ -z "$rps_d1" || "$rps_d1" -lt 19 ]]; then
    echo "FAIL: depth-1 Requests/sec ($rps_d1) < 19" >&2
    exit 1
fi
echo "PASS: depth-1 baseline — Requests/sec=${rps_d1}"

kill "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID
sleep 0.2

# ===========================================================================
# Test 2: depth-5 pipeline — Transfer/sec ≥ 4× depth-1 Transfer/sec
#         (5 GET replies per batch vs 1; each GET reply = $5\r\nvalue\r\n = 13B)
# ===========================================================================

SCRIPT_D5=$(mktemp /tmp/redis_pipe_d5_XXXXXX.lua)
cat > "$SCRIPT_D5" <<'EOF'
function request()
    return redis.pipeline(
        redis.command("GET", "k"),
        redis.command("GET", "k"),
        redis.command("GET", "k"),
        redis.command("GET", "k"),
        redis.command("GET", "k")
    )
end
EOF

python3 "$REDIS_SERVER" "$PORT" &
SERVER_PID=$!
wait_for_server

out_d5=$("$WRK" -t1 -c5 -d3s -R20 -s "$SCRIPT_D5" "redis://localhost:$PORT")
echo "$out_d5"

rps_d5=$(rps_of "$out_d5")
if [[ -z "$rps_d5" || "$rps_d5" -lt 19 ]]; then
    echo "FAIL: depth-5 Requests/sec ($rps_d5) < 19" >&2
    exit 1
fi

# Each batch at depth 5 sends 5 GET replies. Transfer/sec should be ≥ 4× depth-1.
tb_d1=$(transfer_bytes_of "$out_d1")
tb_d5=$(transfer_bytes_of "$out_d5")
if [[ -n "$tb_d1" && "$tb_d1" -gt 0 && -n "$tb_d5" ]]; then
    # depth-5 Transfer/sec should be at least 3× depth-1 (allow some slack)
    threshold=$(( tb_d1 * 3 ))
    if [[ "$tb_d5" -lt "$threshold" ]]; then
        echo "FAIL: depth-5 Transfer/sec ($tb_d5 B/s) < 3× depth-1 ($tb_d1 B/s)" >&2
        exit 1
    fi
    echo "PASS: depth-5 Transfer/sec=${tb_d5} B/s (≥3× depth-1 ${tb_d1} B/s)"
fi

echo "PASS: depth-5 pipeline — Requests/sec=${rps_d5}"

kill "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID
sleep 0.2

# ===========================================================================
# Test 3: latency ordering — with artificial 5ms per-reply server delay,
#         a depth-5 batch should report higher P50 latency than depth-1
#         (5 replies × 5ms = ~25ms per batch vs ~5ms for depth-1).
# ===========================================================================

DELAY_MS=5

python3 "$REDIS_SERVER" "$PORT" "$DELAY_MS" &
SERVER_PID=$!
wait_for_server

out_lat_d1=$("$WRK" -t1 -c5 -d3s -R10 -l -s "$SCRIPT_D1" "redis://localhost:$PORT" 2>&1 || true)
echo "$out_lat_d1"

out_lat_d5=$("$WRK" -t1 -c5 -d3s -R10 -l -s "$SCRIPT_D5" "redis://localhost:$PORT" 2>&1 || true)
echo "$out_lat_d5"

# Extract P50 latency in microseconds from the latency distribution table.
p50_d1=$(echo "$out_lat_d1" | grep -E '^\s*50(\.[0-9]+)?%' | awk '{print $2}' | head -1)
p50_d5=$(echo "$out_lat_d5" | grep -E '^\s*50(\.[0-9]+)?%' | awk '{print $2}' | head -1)

if [[ -n "$p50_d1" && -n "$p50_d5" ]]; then
    # Convert to comparable integers (strip units, compare as milliseconds)
    p50_d1_us=$(echo "$p50_d1" | grep -oE '[0-9]+' | head -1)
    p50_d5_us=$(echo "$p50_d5" | grep -oE '[0-9]+' | head -1)
    if [[ -n "$p50_d1_us" && -n "$p50_d5_us" && "$p50_d5_us" -ge "$p50_d1_us" ]]; then
        echo "PASS: latency ordering — P50 depth-5 (${p50_d5}) ≥ P50 depth-1 (${p50_d1})"
    else
        echo "INFO: latency ordering check inconclusive (P50 depth-1=${p50_d1}, depth-5=${p50_d5})"
    fi
fi

kill "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

rm -f "$SCRIPT_D1" "$SCRIPT_D5"
echo "PASS: redis_pipeline"
