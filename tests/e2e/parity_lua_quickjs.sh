#!/usr/bin/env bash
# tests/e2e/parity_lua_quickjs.sh
#
# Gate D parity test (ADR 0005, Phase 5, t076).
#
# Drives the same logical Redis workload through the LuaJIT and QuickJS
# scripting engines at identical rate/duration and compares the sorted multiset
# of Redis wire commands received by the mock server.  Identical ⇒ PASS.
#
# SKIP conditions:
#   - wrkx binary not found
#   - wrkx built without QuickJS (./configure --with-quickjs)
#   - recording mock or workload scripts missing
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WRK="$ROOT_DIR/wrkx"
MOCK="$SCRIPT_DIR/redis_recording_mock.py"
LUA_SCRIPT="$ROOT_DIR/scripts/redis_get_set.lua"
JS_SCRIPT="$ROOT_DIR/scripts/redis_get_set.js"
PORT=18092

# ---- Guard checks ----------------------------------------------------------

if [[ ! -x "$WRK" ]]; then
    echo "SKIP: wrkx binary not found (build first with 'make')" >&2
    exit 0
fi

if "$WRK" --engine=quickjs -t1 -c1 -d1s -R1 "http://127.0.0.1:1" 2>&1 \
        | grep -q "built without QuickJS"; then
    echo "SKIP: wrkx built without QuickJS (./configure --with-quickjs)" >&2
    exit 0
fi

for f in "$MOCK" "$LUA_SCRIPT" "$JS_SCRIPT"; do
    if [[ ! -f "$f" ]]; then
        echo "SKIP: required file not found: $f" >&2
        exit 0
    fi
done

# ---- Shared helpers ---------------------------------------------------------

LUA_LOG=$(mktemp)
JS_LOG=$(mktemp)
SERVER_PID=""

cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$LUA_LOG" "$JS_LOG"
}
trap cleanup EXIT

wait_for_server() {
    for i in $(seq 1 30); do
        if python3 -c \
            "import socket; s=socket.socket(); s.connect(('127.0.0.1', $PORT)); s.close()" \
            2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "FAIL: recording mock did not start on port $PORT" >&2
    exit 1
}

# Common wrkx flags for both engines (single thread, single connection,
# deterministic rate so request count is identical).
COMMON_FLAGS="-t1 -c1 -d2s -R20"

# ---- Run LuaJIT engine ------------------------------------------------------

python3 "$MOCK" "$PORT" "$LUA_LOG" &
SERVER_PID=$!
wait_for_server

set +e
"$WRK" $COMMON_FLAGS \
    -s "$LUA_SCRIPT" \
    "redis://127.0.0.1:$PORT/" >/dev/null 2>&1
set -e

kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

# ---- Run QuickJS engine -----------------------------------------------------

python3 "$MOCK" "$PORT" "$JS_LOG" &
SERVER_PID=$!
wait_for_server

set +e
"$WRK" $COMMON_FLAGS \
    --engine=quickjs \
    -s "$JS_SCRIPT" \
    "redis://127.0.0.1:$PORT/" >/dev/null 2>&1
set -e

kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

# ---- Normalize and compare --------------------------------------------------

# Sort both logs (ordering across concurrent connections is non-deterministic).
LUA_SORTED=$(sort < "$LUA_LOG")
JS_SORTED=$(sort < "$JS_LOG")

LUA_COUNT=$(echo "$LUA_SORTED" | grep -c . || true)
JS_COUNT=$(echo "$JS_SORTED" | grep -c . || true)

if [[ "$LUA_COUNT" -eq 0 ]]; then
    echo "FAIL: LuaJIT engine produced no commands" >&2
    exit 1
fi
if [[ "$JS_COUNT" -eq 0 ]]; then
    echo "FAIL: QuickJS engine produced no commands" >&2
    exit 1
fi

# Compare the sorted multisets.
if diff <(echo "$LUA_SORTED") <(echo "$JS_SORTED") >/dev/null 2>&1; then
    echo "PASS: parity_lua_quickjs — Lua ($LUA_COUNT cmds) == JS ($JS_COUNT cmds)" \
         "wire-command multisets match"
    exit 0
else
    echo "FAIL: Lua/JS wire-command multisets differ" >&2
    echo "--- LuaJIT output (sorted, first 10 lines) ---" >&2
    echo "$LUA_SORTED" | head -10 >&2
    echo "--- QuickJS output (sorted, first 10 lines) ---" >&2
    echo "$JS_SORTED"  | head -10 >&2
    echo "--- diff (first 20 lines) ---" >&2
    diff <(echo "$LUA_SORTED") <(echo "$JS_SORTED") | head -20 >&2 || true
    exit 1
fi
