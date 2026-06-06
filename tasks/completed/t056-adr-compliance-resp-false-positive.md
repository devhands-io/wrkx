title: Fix adr-compliance.sh false positive on codec files
status: in_progress
adr: 0002

## Problem

CI fails on pushed commits (before t055) with:

    ADR VIOLATION: ADR 0002 §2 — proto resp lacks <proto>_configure()
      offending file: src/proto/resp.c

The `adr-compliance.sh` script globs `src/proto/*.c` and requires every file
to have a `<proto>_configure()` function. `resp.c` is a RESP codec utility,
not a protocol vtable implementation — it correctly has no `resp_configure()`.

## Root cause

The ADR 0002 §2 invariant says:
> "every protocol implementation must expose a <proto>_configure function"

The check should only apply to files that implement the `protocol` vtable
(i.e., expose a `<proto>_protocol()` getter). Codec/utility helpers in
`src/proto/` are not protocol implementations.

## Fix

Update the loop in `scripts/adr-compliance.sh` to skip files that do not
expose a `<proto>_protocol()` function. Only vtable files need `_configure`.

## Acceptance

- `make adr-check` passes on the current tree (after t055 resp.c is gone).
- The check correctly catches a real violation (missing `_configure` in a
  file that does have `<proto>_protocol()`).
- The check correctly skips codec helpers that have no `_protocol()`.
