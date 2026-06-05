#!/usr/bin/env bash
# tests/e2e/cli_output.sh
#
# Guards the five CLI output regressions from t035:
#
#   R1  -v output must include "Credits:" and an event-loop name "[...]"
#   R2  -l flag accepted and produces Latency Distribution without spectrum
#   R3  -L flag produces Latency Distribution WITH detailed spectrum
#   R4  Latency Distribution section appears BEFORE "requests in" summary line
#   R5  Progress bar ("Calibrating:" or "Progress:") appears during a run
#
# Uses the instant mock server (port 18087).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
SERVER="$SCRIPT_DIR/mock_server.py"
PORT=18087

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (build first with 'make')" >&2
    exit 0
fi

PASS=0; FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# ---------------------------------------------------------------------------
# R1 — wrkx -v must print "Credits:" and an event-loop name like "[kqueue]"
# ---------------------------------------------------------------------------
ver=$("$WRK" -v 2>&1 || true)

if echo "$ver" | grep -q "Credits:"; then
    pass "-v includes Credits: line"
else
    fail "-v is missing Credits: line (got: $ver)"
fi

if echo "$ver" | grep -qE '\[.+\]'; then
    pass "-v includes event-loop name [...]"
else
    fail "-v is missing event-loop name like [kqueue] (got: $ver)"
fi

# ---------------------------------------------------------------------------
# Server setup for the run-based checks (R2..R5)
# ---------------------------------------------------------------------------
TMP_L=$(mktemp)   # output from -l run
TMP_BL=$(mktemp)  # output from -L run (big latency / full spectrum)
SERVER_PID=""

cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$TMP_L" "$TMP_BL"
}
trap cleanup EXIT

python3 "$SERVER" "$PORT" instant &
SERVER_PID=$!

for i in $(seq 1 20); do
    if python3 -c \
        "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" \
        2>/dev/null; then
        break
    fi
    sleep 0.1
done

URL="http://localhost:$PORT/"

# ---------------------------------------------------------------------------
# R5 — progress bar must appear in the raw (un-stripped) output.
#      We capture stdout+stderr before stripping \r so the \r-overwritten
#      bar lines are visible as a single long line with embedded \r chars.
# ---------------------------------------------------------------------------
set +e
RAW_L=$("$WRK" -t1 -c5 -d5s -R30 -l "$URL" 2>&1)
RC_L=$?
set -e

if [[ "$RC_L" -ne 0 ]]; then
    fail "wrkx -l exited with code $RC_L"
else
    pass "-l run exited 0"
fi

if printf '%s' "$RAW_L" | grep -q "Calibrating:\|Progress:"; then
    pass "progress bar present (Calibrating: or Progress: found)"
else
    fail "no progress bar found in output (R5)"
fi

# ---------------------------------------------------------------------------
# Sanitise for section-order and spectrum checks: strip \r and leading spaces
# ---------------------------------------------------------------------------
CLEAN_L=$(printf '%s' "$RAW_L" | tr -d '\r')
printf '%s' "$CLEAN_L" > "$TMP_L"

# Run again with -L for spectrum check
set +e
RAW_BL=$("$WRK" -t1 -c5 -d5s -R30 -L "$URL" 2>&1)
RC_BL=$?
set -e

if [[ "$RC_BL" -ne 0 ]]; then
    fail "wrkx -L exited with code $RC_BL"
else
    pass "-L run exited 0"
fi

CLEAN_BL=$(printf '%s' "$RAW_BL" | tr -d '\r')
printf '%s' "$CLEAN_BL" > "$TMP_BL"

# ---------------------------------------------------------------------------
# R2 — -l must produce Latency Distribution but NOT the detailed spectrum
# ---------------------------------------------------------------------------
if grep -q "Latency Distribution (HdrHistogram" "$TMP_L"; then
    pass "-l produces Latency Distribution section"
else
    fail "-l missing Latency Distribution section (R2)"
fi

if grep -q "Detailed Percentile spectrum:" "$TMP_L"; then
    fail "-l must NOT show Detailed Percentile spectrum (R2/R3 boundary)"
else
    pass "-l correctly omits Detailed Percentile spectrum"
fi

# ---------------------------------------------------------------------------
# R3 — -L must produce Latency Distribution AND the detailed spectrum
# ---------------------------------------------------------------------------
if grep -q "Latency Distribution (HdrHistogram" "$TMP_BL"; then
    pass "-L produces Latency Distribution section"
else
    fail "-L missing Latency Distribution section (R3)"
fi

if grep -q "Detailed Percentile spectrum:" "$TMP_BL"; then
    pass "-L includes Detailed Percentile spectrum (R3)"
else
    fail "-L missing Detailed Percentile spectrum (R3)"
fi

# ---------------------------------------------------------------------------
# R4 — Latency Distribution section must appear BEFORE "requests in" line.
#      Use grep -n to get line numbers, then compare.
# ---------------------------------------------------------------------------
dist_line=$(grep -n "Latency Distribution (HdrHistogram" "$TMP_BL" \
    | head -1 | cut -d: -f1 || true)
req_line=$(grep -n "requests in" "$TMP_BL" \
    | head -1 | cut -d: -f1 || true)

if [[ -z "$dist_line" || -z "$req_line" ]]; then
    fail "R4 check inconclusive: dist_line='$dist_line' req_line='$req_line'"
elif [[ "$dist_line" -lt "$req_line" ]]; then
    pass "Latency Distribution appears before 'requests in' (R4) [lines $dist_line vs $req_line]"
else
    fail "Latency Distribution (line $dist_line) appears AFTER 'requests in' (line $req_line) — wrong output order (R4)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "cli_output: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
