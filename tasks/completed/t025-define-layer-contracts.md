title: Define the three layer-contract headers (P1-1)
status: completed
adr: 0001
adr-step: P1-1
depends: []

## Context

ADR 0001 (Three-Layer Engine Architecture) is the authoritative Phase 1 design.
P1-1 is the single gating task: it defines the three C interface contracts that
P1-2, P1-3 and P1-4 implement in parallel. **No implementation in this task — headers
only.** Getting the contracts right and stable here is what makes the three layer
tasks independent and bounded.

The exact signatures are specified in ADR 0001 §"Interface contracts". Reproduce
them faithfully; do not invent fields.

## Produce

- `src/orchestrator.h` — `orchestrator_cfg`, `orchestrator_stats`, the opaque
  `orchestrator` handle, and `orchestrator_create/run/collect/destroy`.
- `src/proto/proto.h` — `proto_status` enum (`PROTO_PENDING/DONE/ERROR`), the
  `protocol` vtable (`name`, `connect`, `write`, `readable`, `close`), and the
  shared `struct connection` (`fd`, opaque `proto_state`, opaque `script_state`).
- `src/scripting/script_api.h` — `script_engine`/`session` opaque types, the
  `script_api` vtable (`create/init/request/response/done/destroy`),
  `script_helper_fn` + `script_helper`, `script_register_helpers`, and the
  `session_create/set/get/destroy` API.

## Steps

- Create the three headers verbatim from ADR 0001 §"Interface contracts".
- Use forward declarations to keep the contracts decoupled (e.g. `proto.h` forward-
  declares nothing it doesn't own; cross-references between `orchestrator.h` and
  `script_api.h`/`proto.h` use the opaque/forward-declared types only).
- Add a stub `src/main.c` that `#include`s all three headers and *declares but does
  not call* anything — its sole job is to prove the headers compile and co-include
  cleanly.
- Add a `make contracts-check` target that compiles the stub `main.c` against the
  three headers (`-fsyntax-only` or compile-to-object), no link step.

## Acceptance (ADR P1-1 completion criterion)

- All three headers compile cleanly against the stub `main.c`.
- No implementation files exist yet (no `orchestrator.c`, `http1.c`, `engine.c`).
- `make contracts-check` is green; `make` / `make test` still green.

## Notes

This contract is **expensive to change after P1-2/3/4 start** (ADR Consequences →
Negative). Review the headers carefully before marking complete.
