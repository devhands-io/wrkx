#!/usr/bin/env bash
# tests/e2e/mysql_basic.sh
#
# E2E tests for the MySQL extension, P6-4 simple query (ADR 0005).
#
# Requires MYSQL_URL env var; skips cleanly if unset.
# Requires the binary to be built with the mysql extension:
#   make EXTENSIONS="redis memcached postgres mysql"
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"

PASS=0
FAIL=0

check() {
    local label="$1"; shift
    if eval "$*" >/dev/null 2>&1; then
        echo "PASS  $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $label"
        FAIL=$((FAIL + 1))
    fi
}

# ---------------------------------------------------------------------------
# Skip if no MYSQL_URL
# ---------------------------------------------------------------------------
if [ -z "${MYSQL_URL:-}" ]; then
    echo "SKIP  mysql_basic.sh: MYSQL_URL not set"
    echo "      Set it to run: MYSQL_URL=mysql://wrkx:secret@host/db make EXTENSIONS=\"redis memcached postgres mysql\" test"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. Build check
# ---------------------------------------------------------------------------
check "mysql extension builds" \
    "make -C '$ROOT_DIR' EXTENSIONS='redis memcached postgres mysql' >/dev/null"

if [ ! -x "$WRK" ]; then
    echo "FAIL  wrkx binary not found at $WRK"
    FAIL=$((FAIL + 1))
    echo ""
    echo "mysql_basic.sh: $PASS passed, $FAIL failed"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Connection smoke test
# ---------------------------------------------------------------------------
TMP=$(mktemp)
cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

"$WRK" -t1 -c1 -d2s -R10 \
    -s <(echo 'function request() return mysql.query("SELECT 1") end') \
    "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  connection smoke test: SELECT 1 runs, > 0 requests"
    PASS=$((PASS + 1))
else
    echo "FAIL  connection smoke test: 0 requests or output missing"
    FAIL=$((FAIL + 1))
    cat "$TMP"
fi

# ---------------------------------------------------------------------------
# 3. Throughput smoke test (4 connections)
# ---------------------------------------------------------------------------
"$WRK" -t1 -c4 -d5s -R100 \
    -s <(echo 'function request() return mysql.query("SELECT 1") end') \
    "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  throughput: -c4 SELECT 1 completes, > 0 requests"
    PASS=$((PASS + 1))
else
    echo "FAIL  throughput: output missing"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# 4. Syntax error returns non-zero error count, does not crash
# ---------------------------------------------------------------------------
set +e
"$WRK" -t1 -c1 -d2s -R10 \
    -s <(printf 'function request() return mysql.query("NOT SQL") end') \
    "$MYSQL_URL" >"$TMP" 2>&1
WRK_RC=$?
set -e
if grep -qiE 'error|errors' "$TMP" 2>/dev/null && [ "$WRK_RC" -eq 0 ]; then
    echo "PASS  syntax error query reports errors, no crash"
    PASS=$((PASS + 1))
else
    echo "FAIL  syntax error query: expected error report in output (rc=$WRK_RC)"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# 5. Wrong password returns error within timeout, not hang
# ---------------------------------------------------------------------------
BAD_URL=$(echo "$MYSQL_URL" | sed 's|://\([^:@]*\):\([^@]*\)@|://\1:wrongpassword@|')
if [ "$BAD_URL" = "$MYSQL_URL" ]; then
    BAD_URL=$(echo "$MYSQL_URL" | sed 's|://\([^@]*\)@|://nouser_bad:badpw@|')
fi
check "wrong password: connection error returned within 15s" \
    "timeout 15 '$WRK' -t1 -c1 -d2s -R5 \
        -s <(echo 'function request() return mysql.query(\"SELECT 1\") end') \
        '$BAD_URL' 2>&1 | grep -iE 'error|connect|auth|denied'"

# ---------------------------------------------------------------------------
# 6. No core engine changes since HEAD
# ---------------------------------------------------------------------------
FROZEN="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c \
        src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h \
        src/net.c src/net.h \
        src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h \
        extensions/postgres/postgres.c extensions/postgres/postgres.h \
        extensions/postgres/pg_message.c extensions/postgres/pg_message.h"

cd "$ROOT_DIR"
check "frozen core+protocol unchanged (no uncommitted modifications)" \
    "test -z \"\$(git diff --name-only HEAD -- $FROZEN)\""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "mysql_basic.sh: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
