#!/usr/bin/env bash
# tests/e2e/postgres_basic.sh
#
# E2E tests for the PostgreSQL extension, P6-1 simple query (ADR 0005).
#
# Requires POSTGRES_URL env var; skips cleanly if unset.
# Requires the binary to be built with the postgres extension:
#   make EXTENSIONS="redis memcached postgres"
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
# Skip if no POSTGRES_URL
# ---------------------------------------------------------------------------
if [ -z "${POSTGRES_URL:-}" ]; then
    echo "SKIP  postgres_basic.sh: POSTGRES_URL not set"
    echo "      Set it to run: POSTGRES_URL=postgres://user:pw@host/db make EXTENSIONS=\"redis memcached postgres\" test"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. Build check
# ---------------------------------------------------------------------------
check "postgres extension builds" \
    "make EXTENSIONS='redis memcached postgres' >/dev/null"

# Verify binary exists and is fresh after the build
if [ ! -x "$WRK" ]; then
    echo "FAIL  wrkx binary not found at $WRK"
    FAIL=$((FAIL + 1))
    echo ""
    echo "postgres_basic.sh: $PASS passed, $FAIL failed"
    [ "$FAIL" -eq 0 ]
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Connection smoke test
# ---------------------------------------------------------------------------
TMP=$(mktemp)
cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

check "connection smoke test: SELECT 1 runs, > 0 requests" \
    "\"$WRK\" -t1 -c1 -d2s -R10 \
        -s <(echo 'function request() return pg.query(\"SELECT 1\") end') \
        \"$POSTGRES_URL\" 2>&1 | tee \"$TMP\" | grep -E 'Requests/sec|requests' | head -1"

# Check that reported requests > 0
smoke_req=$(grep -E 'Requests/sec' "$TMP" 2>/dev/null | awk '{print $2}' | cut -d. -f1 || echo "0")
if [ "${smoke_req:-0}" -gt 0 ] 2>/dev/null; then
    echo "PASS  smoke test reports > 0 req/s ($smoke_req)"
    PASS=$((PASS + 1))
else
    echo "FAIL  smoke test: 0 req/s or output missing"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# 3. Throughput smoke test (4 connections)
# ---------------------------------------------------------------------------
check "throughput: -c4 SELECT 1, 0 errors" \
    "\"$WRK\" -t1 -c4 -d5s -R100 \
        -s <(echo 'function request() return pg.query(\"SELECT 1\") end') \
        \"$POSTGRES_URL\" 2>&1 | tee \"$TMP\" && \
     ! grep -E 'Errors|Non-2xx' \"$TMP\" | grep -v ' 0'"

# ---------------------------------------------------------------------------
# 4. Syntax error returns non-zero error count, does not crash
# ---------------------------------------------------------------------------
set +e
"$WRK" -t1 -c1 -d2s -R10 \
    -s <(printf 'function request() return pg.query("NOT SQL") end') \
    "$POSTGRES_URL" >"$TMP" 2>&1
WRK_RC=$?
set -e
if grep -qE 'Errors|errors' "$TMP" 2>/dev/null && [ "$WRK_RC" -eq 0 ]; then
    echo "PASS  syntax error query reports errors, no crash"
    PASS=$((PASS + 1))
else
    echo "FAIL  syntax error query: expected non-zero error count in output"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# 5. Wrong password returns error within timeout, not hang
# ---------------------------------------------------------------------------
# Construct a URL with a bad password
BAD_URL=$(echo "$POSTGRES_URL" | sed 's|://\([^:@]*\):\([^@]*\)@|://\1:wrongpassword@|')
if [ "$BAD_URL" = "$POSTGRES_URL" ]; then
    # URL has no password — inject a fake user that won't authenticate
    BAD_URL=$(echo "$POSTGRES_URL" | sed 's|://\([^@]*\)@|://nouser_bad:badpw@|')
fi
check "wrong password: connection error returned promptly" \
    "timeout 15 \"$WRK\" -t1 -c1 -d2s -R5 \
        -s <(echo 'function request() return pg.query(\"SELECT 1\") end') \
        \"$BAD_URL\" 2>&1 | grep -Ei 'error|connect|auth'"

# ---------------------------------------------------------------------------
# 6. No core engine changes since HEAD (frozen files are unmodified)
# ---------------------------------------------------------------------------
FROZEN="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c \
        src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h \
        src/net.c src/net.h \
        src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h"

cd "$ROOT_DIR"
check "frozen core+protocol unchanged (no uncommitted modifications)" \
    "test -z \"\$(git diff --name-only HEAD -- $FROZEN)\""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "postgres_basic.sh: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
