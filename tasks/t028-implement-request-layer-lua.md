title: Implement the Request Layer — Lua engine + helpers + session (P1-4)
status: todo
adr: 0001
adr-step: P1-4
depends: t025

## Context

Implements the Request Layer (the "Ammo") behind `src/scripting/script_api.h` (from
t025). It knows nothing about connections or timing: it generates requests and
interprets responses via the scripting hooks, registers per-protocol helpers through
glue modules, and owns per-connection session state. See ADR 0001 §"Layer
responsibilities", §"Glue module pattern", and Invariants 3–4.

Runs in parallel with t026 and t027 once t025 is done. Ships the first concrete
engine (LuaJIT), preserving current scripting behaviour exactly.

## Scope

- **`src/scripting/lua/engine.c`:** implements `script_api` for LuaJIT —
  `create/init/request/response/done/destroy` preserving today's hook semantics
  (`init`, `request`, `response`, `done`) and `wrk.*` API behaviour.
- **Helper registration:** implement `script_register_helpers` and the
  `script_helper` dispatch (engine-agnostic `script_helper_fn(void *engine_ctx)`).
- **Session manager:** `session_create/set/get/destroy` — per-connection
  script-visible key-value store, reachable via `connection.script_state`.
- **Glue module exemplar:** `src/scripting/lua/<proto>_helpers.c` demonstrating the
  pattern (includes both an engine header and a protocol C header — the *only* place
  both may coexist). Use HTTP/1.1 helpers as the worked example.

## Steps

- Port the existing LuaJIT setup/hook invocation into `engine.c` behind the contract.
- Implement the helper-registration namespace mechanism.
- Implement the session KV store.
- Add the example glue module and register its helpers during engine init (NOT from
  `proto/*.c`).

## Acceptance

- Existing Lua scripts behave identically: the `test_script` suite and the Phase 0
  golden-output test pass unchanged.
- **Invariant 3 holds:** `scripting/lua/engine.c` includes no protocol header
  directly. **Invariant 4 holds:** only the glue module includes both a protocol
  header and an engine header.
- `make test` green.
