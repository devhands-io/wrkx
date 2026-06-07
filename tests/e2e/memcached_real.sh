#!/usr/bin/env bash
# tests/e2e/memcached_real.sh — E2E against a real memcached service (t066)
#
# Requires a running memcached server.  Skips gracefully when the service is
# not reachable so the test can be included in the standard suite without
# breaking developer machines that don't run memcached.
#
# Environment variables (all optional):
#   MEMCACHED_HOST  — hostname / IP  (default: 127.0.0.1)
#   MEMCACHED_PORT  — port           (default: 11211)
#
# Local usage:
#   memcached -d && make EXTENSIONS=memcached test-memcached-real
#   # or without make:
#   memcached -d && bash tests/e2e/memcached_real.sh
#
# CI usage:
#   Linux   — memcached service container (see .github/workflows/ci.yml)
#   macOS   — brew-installed daemon started before this step
#
# Exit codes:
#   0 — passed (or skipped because service is not reachable / extension absent)
#   1 — wrkx connected to a real memcached but the test assertions failed
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
GET_SCRIPT="$ROOT_DIR/scripts/memcached_get.lua"
SET_SCRIPT="$ROOT_DIR/scripts/memcached_set_delete.lua"

MC_HOST="${MEMCACHED_HOST:-127.0.0.1}"
MC_PORT="${MEMCACHED_PORT:-11211}"
URL="memcached://${MC_HOST}:${MC_PORT}"

# ---- Extension guard --------------------------------------------------------
if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found at $WRK (run: make EXTENSIONS=memcached)" >&2
    exit 0
fi

probe=$("$WRK" -t1 -c1 -d1s -R1 "memcached://localhost:9" 2>&1 || true)
if echo "$probe" | grep -q 'no extension provides'; then
    echo "SKIP: memcached extension not built — rebuild with EXTENSIONS=memcached" >&2
    exit 0
fi

# ---- Service reachability check ---------------------------------------------
# Distinguish "service not started" (infrastructure) from "protocol failure"
# (extension bug).  SKIP on infrastructure failure; FAIL on protocol failure.
if ! python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(2)
try:
    s.connect(('${MC_HOST}', ${MC_PORT}))
    s.close()
except Exception as e:
    print(f'cannot reach memcached at ${MC_HOST}:${MC_PORT}: {e}', file=sys.stderr)
    sys.exit(1)
" 2>/dev/null; then
    echo "SKIP: memcached not reachable at ${MC_HOST}:${MC_PORT} — start it first" >&2
    echo "      Local: memcached -d -p ${MC_PORT}" >&2
    exit 0
fi

echo "memcached reachable at ${MC_HOST}:${MC_PORT} — running real-service E2E"

# ---- GET workload -----------------------------------------------------------
# -R20 over 3 s: expect ≥19 Requests/sec (within 5%).
echo "--- GET workload (low rate) ---"
output=$("$WRK" -t1 -c5 -d3s -R20 -s "$GET_SCRIPT" "$URL")
echo "$output"

if ! echo "$output" | grep -q "Requests/sec"; then
    echo "FAIL: 'Requests/sec' not found in GET output" >&2
    exit 1
fi
rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 19 ]]; then
    echo "FAIL: GET Requests/sec ($rps) < 19 (expected ≥19 at -R20)" >&2
    exit 1
fi
echo "PASS: GET low-rate — Requests/sec=${rps}"

# ---- SET workload -----------------------------------------------------------
# -R50 over 3 s: expect ≥47 Requests/sec (within 5%).
echo "--- SET/DELETE workload ---"
output=$("$WRK" -t1 -c5 -d3s -R50 -s "$SET_SCRIPT" "$URL")
echo "$output"

if ! echo "$output" | grep -q "Requests/sec"; then
    echo "FAIL: 'Requests/sec' not found in SET output" >&2
    exit 1
fi
rps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' | cut -d. -f1)
if [[ -z "$rps" || "$rps" -lt 47 ]]; then
    echo "FAIL: SET Requests/sec ($rps) < 47 (expected ≥47 at -R50)" >&2
    exit 1
fi
echo "PASS: SET/DELETE — Requests/sec=${rps}"

echo "PASS: memcached_real"
