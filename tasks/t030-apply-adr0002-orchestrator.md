title: Apply ADR 0002 amendments to orchestrator.c
status: todo
adr: 0002
adr-step: orchestrator
depends: t029

## Context

ADR 0002 Decision 1 requires `orchestrator_create` to accept `const script_api *`
as its third parameter. The P1-2 implementation (t026) set `o->api = NULL` because
the vtable was not part of the creation contract at that time. As a result, all
scripting hooks (`request`, `response`, `done`) are unreachable — the orchestrator
calls them NULL-safely but never actually invokes any hook. This task closes that
gap by storing the passed vtable.

The NULL-safety guards added in t026 remain: they protect the unit-test stub path
where `NULL` is intentionally passed for `api`.

## Scope

- **`src/orchestrator.c`**
  - `orchestrator_create`: add `const struct script_api *api` as the third
    parameter (matches the header amendment from t029).
  - Replace `o->api = NULL` with `o->api = api`.
  - No other changes; all NULL-safe guards on `o->api` stay as-is.
  - The `struct orchestrator` definition already contains `const script_api *api`;
    if it was stubbed with a comment, replace or confirm the field exists.

## Steps

1. Edit `orchestrator_create` to accept the new parameter and assign it.
2. Confirm the `struct orchestrator` has a `const script_api *api` field (add it
   if the t026 implementation omitted it).
3. Run `make test-orchestrator`; the nine existing tests pass NULL for api — they
   must still pass unchanged.
4. Verify no new cross-layer include is introduced in orchestrator.c.

## Acceptance

- `make test-orchestrator` exits 0; all 9 tests pass.
- `orchestrator_create` in `orchestrator.c` has four parameters matching
  `orchestrator.h` exactly.
- `o->api` is assigned from the passed value (not hardcoded NULL).
- Invariant 1 preserved: `orchestrator.c` includes only `proto/proto.h` and
  `scripting/script_api.h` (no engine or protocol implementation headers).
