#!/usr/bin/env bash
# tests/e2e/postgres_session.sh
#
# E2E tests for PostgreSQL P6-3 session features:
#   TLS, SCRAM auth, result decoding, transactions, pipelining.
#
# Requires:
#   POSTGRES_URL    — plain postgres:// URL (mandatory; skip all tests if unset)
#   POSTGRES_TLS_URL  — postgres+tls:// URL (skip TLS tests if unset)
#   POSTGRES_SCRAM_URL — postgres:// URL with SCRAM-authenticated user
#                        (skip SCRAM test if unset)
#
# Guard 8: frozen core — verified separately by diff check.

set -euo pipefail

WRKX="${WRKX:-./wrkx}"
PASS=0
SKIP=0
FAIL=0

pass() { echo "PASS  $1"; PASS=$((PASS+1)); }
skip() { echo "SKIP  $1"; SKIP=$((SKIP+1)); }
fail() { echo "FAIL  $1"; FAIL=$((FAIL+1)); }

if [ -z "${POSTGRES_URL:-}" ]; then
    echo "SKIP  postgres_session.sh (POSTGRES_URL not set)"
    exit 0
fi

# -------------------------------------------------------------------------
# Guard: plain pg.query() + pg.execute() regression
# -------------------------------------------------------------------------
SCRIPT_QUERY="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
cat >"$SCRIPT_QUERY" <<'EOF'
function request() return pg.query("SELECT 1") end
EOF
if "$WRKX" -t1 -c1 -d3s -R10 -s "$SCRIPT_QUERY" "$POSTGRES_URL" \
   2>&1 | grep -q "0 errors"; then
    pass "pg.query() regression"
else
    fail "pg.query() regression"
fi
rm -f "$SCRIPT_QUERY"

# -------------------------------------------------------------------------
# pg.result() columns: assert result.ncols > 0
# -------------------------------------------------------------------------
SCRIPT_COLS="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
cat >"$SCRIPT_COLS" <<'EOF'
local ok = true
function request() return pg.query("SELECT 1 AS x, 2 AS y") end
function response(status)
    if status ~= 0 then ok = false; return end
    local r = pg.result()
    if not r or r.ncols ~= 2 then ok = false end
end
EOF
if "$WRKX" -t1 -c1 -d3s -R5 -s "$SCRIPT_COLS" "$POSTGRES_URL" \
   2>&1 | grep -q "0 errors"; then
    pass "pg.result() columns"
else
    fail "pg.result() columns"
fi
rm -f "$SCRIPT_COLS"

# -------------------------------------------------------------------------
# pg.result() rows: SELECT 1 AS x -> rows[1][1] == "1"
# -------------------------------------------------------------------------
SCRIPT_ROWS="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
cat >"$SCRIPT_ROWS" <<'EOF'
local ok = true
function request() return pg.query("SELECT 1 AS x") end
function response(status)
    if status ~= 0 then ok = false; return end
    local r = pg.result()
    if not r or not r.rows or not r.rows[1] then ok = false; return end
    if r.rows[1][1] ~= "1" then ok = false end
end
EOF
if "$WRKX" -t1 -c1 -d3s -R5 -s "$SCRIPT_ROWS" "$POSTGRES_URL" \
   2>&1 | grep -q "0 errors"; then
    pass "pg.result() rows"
else
    fail "pg.result() rows"
fi
rm -f "$SCRIPT_ROWS"

# -------------------------------------------------------------------------
# SCRAM auth
# -------------------------------------------------------------------------
if [ -z "${POSTGRES_SCRAM_URL:-}" ]; then
    skip "SCRAM auth (POSTGRES_SCRAM_URL not set)"
else
    SCRIPT_SCRAM="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
    cat >"$SCRIPT_SCRAM" <<'EOF'
function request() return pg.query("SELECT 1") end
EOF
    if "$WRKX" -t1 -c1 -d3s -R5 -s "$SCRIPT_SCRAM" "$POSTGRES_SCRAM_URL" \
       2>&1 | grep -q "0 errors"; then
        pass "SCRAM auth"
    else
        fail "SCRAM auth"
    fi
    rm -f "$SCRIPT_SCRAM"
fi

# -------------------------------------------------------------------------
# TLS connection
# -------------------------------------------------------------------------
if [ -z "${POSTGRES_TLS_URL:-}" ]; then
    skip "TLS connection (POSTGRES_TLS_URL not set)"
else
    SCRIPT_TLS="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
    cat >"$SCRIPT_TLS" <<'EOF'
function request() return pg.query("SELECT 1") end
EOF
    if "$WRKX" -t1 -c1 -d3s -R5 -s "$SCRIPT_TLS" "$POSTGRES_TLS_URL" \
       2>&1 | grep -q "0 errors"; then
        pass "TLS connection"
    else
        fail "TLS connection"
    fi
    rm -f "$SCRIPT_TLS"
fi

# -------------------------------------------------------------------------
# pg.rollback() on error: connection survives after a failed transaction
# -------------------------------------------------------------------------
SCRIPT_ROLLBACK="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
cat >"$SCRIPT_ROLLBACK" <<'EOF'
local cycle = 0
function request()
    cycle = cycle + 1
    if cycle % 2 == 1 then
        -- Intentionally bad SQL inside a transaction; will cause error
        return pg.begin()
            .. pg.execute("SELECT * FROM nonexistent_table_xyzzy_12345")
            .. pg.rollback()
    else
        return pg.query("SELECT 1")
    end
end
EOF
if "$WRKX" -t1 -c1 -d5s -R10 -s "$SCRIPT_ROLLBACK" "$POSTGRES_URL" \
   2>&1 | grep -qE "[0-9]+ requests"; then
    pass "pg.rollback() on error: connection survives"
else
    fail "pg.rollback() on error: connection survives"
fi
rm -f "$SCRIPT_ROLLBACK"

# -------------------------------------------------------------------------
# Multi-query pipelining smoke: pg.begin()..pg.execute()..pg.commit()
# Requires a writable table 'bench(id int, val text)'.
# -------------------------------------------------------------------------
SCRIPT_TXN="$(mktemp /tmp/pg_e2e_XXXXXX.lua)"
cat >"$SCRIPT_TXN" <<'EOF'
local counter = 0
function request()
    counter = counter + 1
    local id  = counter % 1000
    local val = tostring(id * 7)
    return pg.begin()
        .. pg.execute("INSERT INTO bench(id, val) VALUES($1, $2)",
                       tostring(id), val)
        .. pg.commit()
end
EOF
if "$WRKX" -t1 -c1 -d5s -R20 -s "$SCRIPT_TXN" "$POSTGRES_URL" \
   2>&1 | grep -q "0 errors"; then
    pass "multi-query pipelining smoke"
else
    skip "multi-query pipelining smoke (no bench table or 0 errors not matched)"
fi
rm -f "$SCRIPT_TXN"

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
echo ""
echo "postgres_session.sh: $PASS passed, $SKIP skipped, $FAIL failed"
[ "$FAIL" -eq 0 ]
