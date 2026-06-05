#!/usr/bin/env bash
# scripts/compare.sh — run the frozen phase-0 binary and the new binary against
# the SAME mock server + args, then compare key metrics side by side.
#
# Usage:
#   scripts/compare.sh [MODE] [-- wrk args...]
#
#   MODE      mock_server.py mode: instant|delay|flaky|drop|close  (default: instant)
#   wrk args  passed verbatim to BOTH binaries (default: -t4 -c16 -d5s -R2000 -l)
#
# Examples:
#   scripts/compare.sh                       # instant, default args
#   scripts/compare.sh close                 # Connection: close server
#   scripts/compare.sh delay -- -t2 -c8 -d8s -R1000 -L
#
# Output is never bit-identical (timing), so this compares within tolerance:
#   - Requests/sec: NEW must be within TOL% of OLD
#   - 50% latency:  reported for eyeballing (not gated by default)
# Exit 0 if within tolerance, 1 otherwise. Intended for manual A/B and CI gating.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OLD="$ROOT/baseline/wrkx0"
NEW="$ROOT/wrkx"
SERVER="$ROOT/tests/e2e/mock_server.py"
PORT=18099
TOL=25   # allowed % divergence of NEW Requests/sec from OLD

MODE="instant"
if [[ $# -gt 0 && "$1" != "--" ]]; then MODE="$1"; shift; fi
[[ "${1:-}" == "--" ]] && shift
ARGS=("$@")
[[ ${#ARGS[@]} -eq 0 ]] && ARGS=(-t4 -c16 -d5s -R2000 -l)

for b in "$OLD" "$NEW"; do
    if [[ ! -x "$b" ]]; then
        echo "missing binary: $b" >&2
        echo "build with: make && make baseline" >&2
        exit 2
    fi
done

SERVER_PID=""
cleanup() { [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT

python3 "$SERVER" "$PORT" "$MODE" &
SERVER_PID=$!
for i in $(seq 1 20); do
    python3 -c "import socket;s=socket.socket();s.connect(('127.0.0.1',$PORT));s.close()" 2>/dev/null && break
    sleep 0.1
done

URL="http://localhost:$PORT/"

run() {  # $1=binary -> prints "rps|p50"
    local out
    out=$("$1" "${ARGS[@]}" "$URL" 2>/dev/null | tr -d '\r')
    local rps p50
    rps=$(printf '%s\n' "$out" | awk '/Requests\/sec:/{print $2; exit}')
    p50=$(printf '%s\n' "$out" | awk '/ 50\.000%/{print $2; exit}')
    echo "${rps:-0}|${p50:-n/a}"
}

echo "== compare: mode=$MODE args=[${ARGS[*]}] (tol ${TOL}%) =="
r_old=$(run "$OLD"); r_new=$(run "$NEW")
rps_old=${r_old%|*}; p50_old=${r_old#*|}
rps_new=${r_new%|*}; p50_new=${r_new#*|}

printf '%-18s %14s %14s\n' "metric" "OLD(wrkx0)" "NEW(wrkx)"
printf '%-18s %14s %14s\n' "Requests/sec" "$rps_old" "$rps_new"
printf '%-18s %14s %14s\n' "Latency p50"  "$p50_old" "$p50_new"

# Tolerance check on Requests/sec (integer math on truncated values).
o=${rps_old%.*}; n=${rps_new%.*}
if [[ -z "$o" || "$o" -eq 0 ]]; then
    echo "RESULT: inconclusive (OLD rps=$rps_old)"; exit 1
fi
diff=$(( n > o ? n - o : o - n ))
pct=$(( diff * 100 / o ))
if [[ "$pct" -le "$TOL" ]]; then
    echo "RESULT: PASS — NEW within ${pct}% of OLD"
    exit 0
else
    echo "RESULT: FAIL — NEW diverges ${pct}% from OLD (> ${TOL}%)"
    exit 1
fi
