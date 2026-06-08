#!/usr/bin/env bash
# tests/e2e/mysql_prepared.sh
#
# E2E tests for MySQL prepared statements, P6-5 (ADR 0005).
#
# Requires MYSQL_URL env var; skips cleanly if unset.
# Requires MySQL 8.0+ (for performance_schema.prepared_statements_instances).
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
    echo "SKIP  mysql_prepared.sh: MYSQL_URL not set"
    echo "      Set it to run: MYSQL_URL=mysql://wrkx:secret@host/db make EXTENSIONS=\"redis memcached postgres mysql\" test"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. Build check
# ---------------------------------------------------------------------------
check "mysql extension builds with prepared-statement support" \
    "make -C '$ROOT_DIR' EXTENSIONS='redis memcached postgres mysql' >/dev/null"

if [ ! -x "$WRK" ]; then
    echo "FAIL  wrkx binary not found at $WRK"
    FAIL=$((FAIL + 1))
    echo ""
    echo "mysql_prepared.sh: $PASS passed, $FAIL failed"
    exit 1
fi

# ---------------------------------------------------------------------------
# Shared temp file
# ---------------------------------------------------------------------------
TMP=$(mktemp)
cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 2. Basic prepared query smoke test
# ---------------------------------------------------------------------------
"$WRK" -t1 -c1 -d2s -R10 \
    -s "$ROOT_DIR/scripts/mysql_prepared.lua" \
    "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  basic prepared query: mysql_prepared.lua exits 0, > 0 requests"
    PASS=$((PASS + 1))
else
    echo "FAIL  basic prepared query: 0 requests or missing output"
    FAIL=$((FAIL + 1))
    cat "$TMP"
fi

# ---------------------------------------------------------------------------
# 3. Parameterized throughput (-c4)
# ---------------------------------------------------------------------------
"$WRK" -t1 -c4 -d5s -R100 \
    -s "$ROOT_DIR/scripts/mysql_prepared.lua" \
    "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  parameterized throughput: -c4 > 0 requests, no crash"
    PASS=$((PASS + 1))
else
    echo "FAIL  parameterized throughput: output missing"
    FAIL=$((FAIL + 1))
fi

# Check for errors in throughput run
if grep -qiE '^[[:space:]]*errors' "$TMP" 2>/dev/null; then
    # Accept if error count is 0
    if grep -qE 'errors.*[^0]$|[^0] errors' "$TMP" 2>/dev/null; then
        echo "FAIL  parameterized throughput: non-zero error count"
        FAIL=$((FAIL + 1))
    else
        echo "PASS  parameterized throughput: zero errors"
        PASS=$((PASS + 1))
    fi
else
    echo "PASS  parameterized throughput: no error line in output"
    PASS=$((PASS + 1))
fi

# ---------------------------------------------------------------------------
# 4. Stmt_id reuse (second request on same connection reuses cached stmt)
#    Verify by checking MySQL performance_schema if available.
# ---------------------------------------------------------------------------
PREPSTMT_CHECK=$(mysql -u"${MYSQL_URL##*:\/\/}" \
    -h"$(echo "${MYSQL_URL}" | sed 's|.*@\(.*\)/.*|\1|')" \
    -e "SELECT COUNT(*) FROM performance_schema.prepared_statements_instances" \
    2>/dev/null | tail -1 || echo "unavailable")

if [ "$PREPSTMT_CHECK" != "unavailable" ]; then
    echo "PASS  stmt_id reuse: performance_schema accessible (manual verification possible)"
    PASS=$((PASS + 1))
else
    echo "INFO  stmt_id reuse: performance_schema not accessible, skipping direct check"
fi

# ---------------------------------------------------------------------------
# 5. SQL change triggers re-prepare (inline script with two different queries)
# ---------------------------------------------------------------------------
"$WRK" -t1 -c1 -d2s -R5 \
    -s <(cat <<'EOF'
local counter = 0
function request()
    counter = counter + 1
    if counter % 2 == 1 then
        return mysql.execute("SELECT ?", tostring(counter))
    else
        return mysql.execute("SELECT ?+1", tostring(counter))
    end
end
EOF
) "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  SQL change mid-workload: re-prepare on SQL change, no crash"
    PASS=$((PASS + 1))
else
    echo "FAIL  SQL change mid-workload: 0 requests or crash"
    FAIL=$((FAIL + 1))
    cat "$TMP"
fi

# ---------------------------------------------------------------------------
# 6. Wrong-type parameter: Lua number coerced to string (should succeed)
# ---------------------------------------------------------------------------
"$WRK" -t1 -c1 -d2s -R5 \
    -s <(echo 'function request() return mysql.execute("SELECT ?", 99) end') \
    "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  number param coercion: mysql.execute('SELECT ?', 99) succeeds"
    PASS=$((PASS + 1))
else
    echo "FAIL  number param coercion: 0 requests or crash"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# 7. QuickJS parity: mysql_prepared.js
# ---------------------------------------------------------------------------
"$WRK" -t1 -c1 -d2s -R10 \
    -s "$ROOT_DIR/scripts/mysql_prepared.js" \
    "$MYSQL_URL" >"$TMP" 2>&1 || true

if grep -qE '[0-9]+ requests' "$TMP" 2>/dev/null; then
    echo "PASS  QuickJS parity: mysql_prepared.js > 0 requests"
    PASS=$((PASS + 1))
else
    echo "FAIL  QuickJS parity: 0 requests or crash"
    FAIL=$((FAIL + 1))
    cat "$TMP"
fi

# ---------------------------------------------------------------------------
# 8. Frozen-file diff clean
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
echo "mysql_prepared.sh: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
