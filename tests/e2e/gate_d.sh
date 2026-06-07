#!/usr/bin/env bash
# tests/e2e/gate_d.sh
#
# Gate D confirmation (ADR 0005, Phase 5, t076).
#
# Verifies that the Request Layer abstraction is not Lua-shaped:
#  1. The frozen protocol + orchestrator core is unchanged since the P5-2
#     baseline tag (p5-baseline = commit 0502931).
#  2. Both the LuaJIT and QuickJS engines build successfully.
#  3. The same Redis workload produces identical wire output from both engines.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BASELINE="${GATE_D_BASELINE:-p5-baseline}"
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
# 1. Frozen core + protocol paths must be unchanged since the p5 baseline.
# ---------------------------------------------------------------------------
FROZEN="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h \
        src/net.c src/net.h src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h"

cd "$ROOT_DIR"
check "frozen core+protocol unchanged since $BASELINE" \
    "test -z \"\$(git diff --name-only $BASELINE HEAD -- $FROZEN)\""

# ---------------------------------------------------------------------------
# 2. Both engines build.
# ---------------------------------------------------------------------------
check "LuaJIT build" \
    "make EXTENSIONS=redis >/dev/null"

check "QuickJS build" \
    "make QUICKJS_ENABLED=1 EXTENSIONS=redis >/dev/null"

# ---------------------------------------------------------------------------
# 3. Lua/JS Redis request parity.
# ---------------------------------------------------------------------------
check "Lua/JS Redis request parity" \
    "bash tests/e2e/parity_lua_quickjs.sh"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "Gate D: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
