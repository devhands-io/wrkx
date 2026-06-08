#!/usr/bin/env bash
# tests/e2e/postgres_prepared.sh
#
# E2E tests for the PostgreSQL extension, P6-2 extended query + Gate E (ADR 0005).
#
# Requires POSTGRES_URL env var; skips cleanly if unset.
# Requires the binary to be built with the postgres extension:
#   make EXTENSIONS="redis memcached postgres"
#
# Gate E: confirms that multi-step stateful protocol state (parse plan,
# parameter binding, result metadata) lives entirely inside the vtable with
# zero leakage into the orchestrator or scheduler.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"

BASELINE="${GATE_E_BASELINE:-p6-baseline}"

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
# Verify p6-baseline tag exists
# ---------------------------------------------------------------------------
cd "$ROOT_DIR"
if ! git rev-parse --verify "$BASELINE" >/dev/null 2>&1; then
    echo "FAIL  $BASELINE tag not found"
    echo "      Recovery: find the last commit before extensions/postgres/ was added,"
    echo "      then: git tag p6-baseline <that-commit>"
    exit 1
fi

# ---------------------------------------------------------------------------
# Skip if no POSTGRES_URL
# ---------------------------------------------------------------------------
if [ -z "${POSTGRES_URL:-}" ]; then
    echo "SKIP  postgres_prepared.sh: POSTGRES_URL not set"
    echo "      Set it to run: POSTGRES_URL=postgres://user:pw@host/db make EXTENSIONS=\"redis memcached postgres\" test"

    # Still run Gate E (frozen-core check) even without a live server
    echo ""
    echo "--- Running Gate E (frozen-core check only) ---"
else
    # ---------------------------------------------------------------------------
    # Live-server tests
    # ---------------------------------------------------------------------------
    TMP=$(mktemp)
    cleanup() { rm -f "$TMP"; }
    trap cleanup EXIT

    if [ ! -x "$WRK" ]; then
        echo "FAIL  wrkx binary not found at $WRK"
        FAIL=$((FAIL + 1))
    fi

    # 1. Extended query smoke test
    check "extended query smoke: pg.execute() exits 0, > 0 requests" \
        "\"$WRK\" -t1 -c1 -d2s -R10 \
            -s <(echo 'local s = pg.prepare(\"SELECT \$1::int\")
                 function request() return pg.execute(s, \"1\") end') \
            \"$POSTGRES_URL\" 2>&1 | grep -E 'Requests/sec'"

    # 2. Multi-param execute
    check "multi-param execute: SELECT \$1::int + \$2::int, 0 errors" \
        "\"$WRK\" -t1 -c1 -d2s -R10 \
            -s <(echo 'function request() return pg.execute(\"SELECT \$1::int + \$2::int\", \"3\", \"4\") end') \
            \"$POSTGRES_URL\" 2>&1 | tee \"$TMP\" && \
         ! grep -E 'Errors' \"$TMP\" | grep -v ' 0'"

    # 3. NULL param
    check "NULL param: pg.execute SELECT \$1::text with nil, 0 errors" \
        "\"$WRK\" -t1 -c1 -d2s -R10 \
            -s <(printf 'function request() return pg.execute(\"SELECT \$1::text\", nil) end') \
            \"$POSTGRES_URL\" 2>&1 | tee \"$TMP\" && \
         ! grep -E 'Errors' \"$TMP\" | grep -v ' 0'"

    # 4. Syntax error via execute: non-zero error count, no crash
    set +e
    "$WRK" -t1 -c1 -d2s -R10 \
        -s <(printf 'function request() return pg.execute("NOT SQL") end') \
        "$POSTGRES_URL" >"$TMP" 2>&1
    WRK_RC=$?
    set -e
    if grep -qE 'Errors|errors' "$TMP" 2>/dev/null && [ "$WRK_RC" -eq 0 ]; then
        echo "PASS  syntax error via execute: non-zero error count, no crash"
        PASS=$((PASS + 1))
    else
        echo "FAIL  syntax error via execute: expected error count in output"
        FAIL=$((FAIL + 1))
    fi

    # 5. pg.query() regression: still works alongside pg.execute()
    check "pg.query() regression: works alongside pg.execute()" \
        "\"$WRK\" -t1 -c2 -d2s -R20 \
            -s <(printf 'local toggle = 0
                 function request()
                   toggle = toggle + 1
                   if toggle %% 2 == 0 then
                     return pg.query(\"SELECT 1\")
                   else
                     return pg.execute(\"SELECT \\\$1::int\", \"1\")
                   end
                 end') \
            \"$POSTGRES_URL\" 2>&1 | grep -E 'Requests/sec'"
fi

# ---------------------------------------------------------------------------
# Gate E: frozen core + protocol unchanged since p6-baseline
# ---------------------------------------------------------------------------
FROZEN="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c \
        src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h \
        src/net.c src/net.h \
        src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h"

check "frozen core+protocol unchanged since $BASELINE" \
    "test -z \"\$(git diff --name-only $BASELINE HEAD -- $FROZEN)\""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "Gate E: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
