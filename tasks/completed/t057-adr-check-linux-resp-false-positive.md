title: Fix adr-check Linux CI failure — resp.c false positive
status: completed
adr: 0002
fixed-by: t056 (commit e686e93)

## Problem

Linux CI job fails with:

    ADR VIOLATION: ADR 0002 §2 — proto resp lacks <proto>_configure()
      offending file: /home/runner/work/wrkx/wrkx/src/proto/resp.c

Same root cause as t056 (macOS runner). The `adr-compliance.sh` script
required every `src/proto/*.c` file to have `<proto>_configure()`. `resp.c`
is a RESP codec utility without a protocol vtable — it correctly has no
`resp_configure()`.

## Resolution

Fixed by t056: `scripts/adr-compliance.sh` now gates the ADR 0002 §2 check
on the presence of a `<proto>_protocol()` getter. Codec helpers without that
function are exempt. Will resolve both the macOS and Linux CI failures once
commits are pushed.
