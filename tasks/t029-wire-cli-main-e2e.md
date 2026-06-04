title: Wire CLI + main, full E2E suite green (P1-5)
status: todo
adr: 0001
adr-step: P1-5
depends: t026, t027, t028

## Context

The join point of Phase 1. With the three layers implemented (t026/t027/t028), this
task wires them together at startup and proves the refactor is behaviour-preserving.
This is the only Phase 1 task that legitimately touches all three layers — it is the
wiring, not a layer. See ADR 0001 §"Implementation Sequence" and §"Phase 1 Migration
Map" (CLI / wiring row).

## Scope (Migration-Map symbols this task owns)

- **`src/cli.c` (+ `cli.h`):** `usage`, `longopts[]`, `parse_args`, `copy_url_part`
  → builds an `orchestrator_cfg`.
- **`src/main.c`:** replaces the stub from t025. Parses args, selects the protocol
  (HTTP/1.1) and scripting engine (Lua), then calls
  `orchestrator_create(cfg, protocol, script_engine)` → `orchestrator_run` →
  `orchestrator_collect` → `orchestrator_destroy`.
- Reduce `src/wrk.c` to wiring only, or remove it entirely once all symbols have
  moved per the Migration Map.

## Steps

- Implement `cli.c` producing `orchestrator_cfg` from argv.
- Implement `main.c` selecting protocol + engine and driving the orchestrator
  lifecycle.
- Remove now-dead code from `wrk.c` / `wrk.h`; update the Makefile object list and
  drop `make contracts-check`'s stub-only assumption (real `main.c` now links).
- Run the full suite and diff observable output against the pre-refactor baseline.

## Acceptance

- Full E2E suite green: latency accuracy (t011), reconnect robustness (t012), error
  counting (t013).
- `wrkx` observable behaviour and stdout are **identical** to pre-refactor (Phase 0
  golden-output test passes).
- All 42 unit tests + E2E pass on macOS (Clang) and Linux CI (GCC).
- `wrk.c` is wiring-only or gone; no Migration-Map symbol is orphaned.
- `make test` green.
