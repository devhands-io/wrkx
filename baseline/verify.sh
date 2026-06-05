#!/usr/bin/env bash
# baseline/verify.sh
#
# Verifies the frozen phase-0 code under baseline/src/ is byte-for-byte
# unchanged against baseline/MANIFEST.sha256.
#
# Exit 0 = unchanged (OK); exit 1 = drift detected (a file changed, was added,
# or was removed). This is the hash guard required by the baseline policy: the
# phase-0 reference must stay untouched until the new orchestrator/engine
# architecture is fully validated. See baseline/README.md.
#
# Portable across macOS (shasum) and Linux (sha256sum). Run from anywhere.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/src"
MANIFEST="$DIR/MANIFEST.sha256"

if [[ ! -f "$MANIFEST" ]]; then
    echo "baseline-verify: FAIL — manifest missing: $MANIFEST" >&2
    exit 1
fi

# Pick a checksum tool.
if command -v shasum >/dev/null 2>&1; then
    SHACMD="shasum -a 256"
elif command -v sha256sum >/dev/null 2>&1; then
    SHACMD="sha256sum"
else
    echo "baseline-verify: FAIL — no shasum/sha256sum available" >&2
    exit 1
fi

rc=0

# 1) Every manifest entry must match (catches edits + deletions).
#    The manifest stores paths relative to baseline/src, so verify from there.
if ! ( cd "$SRC" && $SHACMD -c "$MANIFEST" ) >/tmp/baseline_verify.$$ 2>&1; then
    rc=1
fi
grep -v ': OK$' /tmp/baseline_verify.$$ >/tmp/baseline_verify_bad.$$ 2>/dev/null || true
if [[ -s /tmp/baseline_verify_bad.$$ ]]; then
    echo "baseline-verify: FAIL — frozen phase-0 code changed:" >&2
    sed 's/^/  /' /tmp/baseline_verify_bad.$$ >&2
    rc=1
fi
rm -f /tmp/baseline_verify.$$ /tmp/baseline_verify_bad.$$

# 2) No NEW files may appear under baseline/src (catches additions the manifest
#    doesn't know about).
actual=$( ( cd "$SRC" && find . -type f | sed 's|^\./||' | sort ) )
expected=$( awk '{print $2}' "$MANIFEST" | sort )
extra=$(comm -23 <(printf '%s\n' "$actual") <(printf '%s\n' "$expected") || true)
if [[ -n "$extra" ]]; then
    echo "baseline-verify: FAIL — unexpected file(s) under baseline/src:" >&2
    printf '  %s\n' $extra >&2
    rc=1
fi

if [[ "$rc" -eq 0 ]]; then
    echo "baseline-verify: OK — phase-0 code unchanged ($(wc -l < "$MANIFEST" | tr -d ' ') files)"
fi
exit "$rc"
