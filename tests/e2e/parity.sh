#!/usr/bin/env bash
# tests/e2e/parity.sh
#
# Old-vs-new behavioural parity gate (ADR 0003 §Compliance).
#
# Builds nothing itself — assumes `wrkx` and `baseline/wrkx0` already exist
# (the Makefile `compare`/test wiring builds them). Runs scripts/compare.sh
# against two server profiles and fails if the new binary diverges from the
# frozen phase-0 baseline:
#
#   instant  - clean keep-alive: Requests/sec must match within tolerance,
#              and NEW must introduce no socket errors (t037).
#   kalimit  - keep-alive with a keepalive_requests-style close every N
#              responses: same rps tolerance AND NEW socket errors <= OLD,
#              i.e. the graceful close must not be miscounted as a read error
#              (t038).
#
# Reverting either t037 or t038 makes this fail.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPARE="$ROOT/scripts/compare.sh"
OLD="$ROOT/baseline/wrkx0"
NEW="$ROOT/wrkx"

if [[ ! -x "$NEW" || ! -x "$OLD" ]]; then
    echo "SKIP: need both $NEW and $OLD (run: make && make baseline)" >&2
    exit 0
fi

rc=0
# Short runs keep CI fast; the defects show immediately. -L gives the p50 row.
echo "### parity: instant (clean keep-alive) ###"
bash "$COMPARE" instant -- -t4 -c16 -d5s -R2000 -L || rc=1
echo
# Close every 100 responses so each connection crosses the limit several times
# in a short run (~10000 reqs / 16 conns = ~625/conn -> ~6 closes/conn). Without
# t038 these closes surface as NEW read errors; with it, zero.
echo "### parity: kalimit (close every 100 responses, like nginx keepalive_requests) ###"
SERVER_ARG=100 bash "$COMPARE" kalimit -- -t4 -c16 -d5s -R2000 -L || rc=1

echo
if [[ "$rc" -eq 0 ]]; then
    echo "PASS: parity (rps + socket-error parity with phase-0 baseline)"
else
    echo "FAIL: parity divergence from phase-0 baseline"
fi
exit "$rc"
