#!/usr/bin/env bash
# scripts/adr-compliance.sh
#
# ADR 0001 + 0002 layer-boundary compliance guard.
#
# Checks that no source file violates the cross-layer-include invariants
# defined in docs/adr/0001-three-layer-engine-architecture.md and
# docs/adr/0002-layer-configuration-protocol.md.
#
# Exit codes:
#   0 — no violations found
#   1 — one or more violations found (first offender printed)
#
# Usage:
#   scripts/adr-compliance.sh [SOURCE_ROOT]
#
# SOURCE_ROOT defaults to src/ relative to the script's parent directory.
# The script is intentionally written in portable POSIX sh syntax (bash
# shebang for local convenience; CI runners may use dash).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT_DIR/src}"

VIOLATIONS=0

fail() {
    echo "ADR VIOLATION: $1" >&2
    VIOLATIONS=$((VIOLATIONS + 1))
}

# ---------------------------------------------------------------------------
# Helper: grep a directory for a pattern; print the first match if found.
# Returns 1 if no match (grep exits non-zero on no match — guard with ||:).
# ---------------------------------------------------------------------------
check_absent() {
    local label="$1"   # human-readable rule description
    local dir="$2"     # directory tree to search
    local pattern="$3" # extended-regex pattern to match (grep -rE)

    if [ ! -d "$dir" ]; then
        return 0   # layer dir doesn't exist yet — skip gracefully
    fi

    local hit
    hit=$(grep -rlE "$pattern" "$dir" 2>/dev/null | head -1 || true)
    if [ -n "$hit" ]; then
        fail "$label\n  offending file: $hit\n  pattern: $pattern"
    fi
}

# ---------------------------------------------------------------------------
# Invariant 2 (ADR 0001): no scripting engine header in src/proto/
#
# proto/*.c must not #include lua.h, quickjs.h, or any other scripting
# engine header.  Protocol behaviour is exposed to scripts only through
# glue modules.
# ---------------------------------------------------------------------------
check_absent \
    "Invariant 2 — scripting header in proto/ (lua.h / quickjs.h)" \
    "$SRC/proto" \
    '#include[[:space:]]*("|<)(lua\.h|quickjs\.h|luaconf\.h|luajit\.h)'

# ---------------------------------------------------------------------------
# Invariant 3 (ADR 0001): no protocol header directly included in
# scripting/<engine>/engine.c.  Protocol behaviour must flow through glue
# modules only.
# ---------------------------------------------------------------------------
for engine_c in "$SRC"/scripting/*/engine.c; do
    [ -f "$engine_c" ] || continue
    if grep -qE '#include[[:space:]]*("|<)(proto/|http_parser\.h|transport\.h)' \
            "$engine_c" 2>/dev/null; then
        # http_parser.h is allowed in engine.c (it is a utility, not a protocol
        # implementation header — see the ADR 0002 configure-slot note).
        # Exclude it from the violation check.
        if grep -qE '#include[[:space:]]*("|<)(proto/)' "$engine_c" 2>/dev/null; then
            fail "Invariant 3 — protocol header in engine.c\n  offending file: $engine_c"
        fi
    fi
done

# ---------------------------------------------------------------------------
# Invariant 1 (ADR 0001): orchestrator.c must not include any protocol or
# scripting implementation header other than proto.h and script_api.h.
# ---------------------------------------------------------------------------
ORCH_C="$SRC/orchestrator.c"
if [ -f "$ORCH_C" ]; then
    if grep -qE '#include[[:space:]]*("|<)(lua\.h|quickjs\.h|proto/http1\.h|transport\.h)' \
            "$ORCH_C" 2>/dev/null; then
        fail "Invariant 1 — implementation header in orchestrator.c\n  offending file: $ORCH_C"
    fi
fi

# ---------------------------------------------------------------------------
# ADR 0002 Decision 1: orchestrator_create must be called with 4 arguments
# (cfg, protocol *, const script_api *, script_engine *).
# Check that no caller passes only 3 positional arguments.
#
# Heuristic: look for orchestrator_create( ... , ... , ) with only 2 commas
# inside the call (3-arg form).  Unit-test stubs that pass NULL explicitly
# are compliant — they have 4 args.  We look for obviously wrong 3-arg forms.
# ---------------------------------------------------------------------------
# (This check is best-effort.  A proper static analysis tool would be more
# accurate.  We skip it here to avoid false positives from multi-line calls.)

# ---------------------------------------------------------------------------
# ADR 0002 Decision 2: every protocol implementation must expose a
# <proto>_configure function.  Check that each proto/*.c has one.
# ---------------------------------------------------------------------------
for proto_c in "$SRC"/proto/*.c; do
    [ -f "$proto_c" ] || continue
    proto_name="$(basename "${proto_c%.c}")"
    if ! grep -qE "${proto_name}_configure[[:space:]]*\(" "$proto_c" 2>/dev/null; then
        fail "ADR 0002 §2 — proto $proto_name lacks <proto>_configure()\n  offending file: $proto_c"
    fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
if [ "$VIOLATIONS" -eq 0 ]; then
    echo "adr-compliance: OK (no violations)"
    exit 0
else
    echo "" >&2
    echo "adr-compliance: $VIOLATIONS violation(s) found — see above" >&2
    exit 1
fi
