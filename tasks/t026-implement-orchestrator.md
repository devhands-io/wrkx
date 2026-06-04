title: Implement the Orchestrator layer (P1-2)
status: todo
adr: 0001
adr-step: P1-2
depends: t025

## Context

Implements the Orchestrator layer behind `src/orchestrator.h` (from t025). The
Orchestrator is the "Tank": it knows nothing about protocols or request content. It
asks the Protocol Engine one question (*"is the response complete?"*) and the Request
Layer one question (*"what bytes do I send next?"*). See ADR 0001 §"Layer
responsibilities" and §"Phase 1 Migration Map".

Runs in parallel with t027 and t028 once t025 is done.

## Scope (Migration-Map symbols this task owns)

- **Thread pool + lifecycle:** `thread_main`, `progress_main`, `handler` (SIGINT,
  signal-driven drain). Lifecycle `init → connect → run → drain → report`.
- **ae event-loop glue:** `socket_writeable` / `socket_readable` become thin glue
  that delegates to `proto->write` / `proto->readable`.
- **Rate controller (`src/rate.c` sub-module):** `usec_to_next_send`,
  `delay_request`, `calibrate`, `check_timeouts`, `sample_rate` + Coordinated-Omission
  correction (HdrHistogram).
- **Stats aggregation:** latency percentiles, RPS, error counts → `orchestrator_stats`.
- **Report stage:** `print_stats_header`, `print_units`, `print_stats`,
  `print_hdr_latency` (CLI-side reporter).
- **State:** `cfg`, `statistics`, `stop`, `g_calibrated_threads`, `g_progress_done`
  fold into the **opaque `orchestrator` handle** — no `globals.h`.
- **Shared util:** `time_us` → `src/utils.h`.

> A layer is a boundary, not a file: the Orchestrator may span `orchestrator.c`,
> `rate.c`, and the existing `stats.c`. Keep each focused — do not recreate a god-file.

## Steps

- Implement `orchestrator_create/run/collect/destroy` per the contract.
- Extract the rate-controller symbols into `src/rate.c` / `src/rate.h`.
- Move `time_us` to `src/utils.h`.
- Drive connections purely through the `protocol` vtable and `script_api` — no HTTP
  or Lua specifics anywhere in this layer.
- Provide a **stub protocol** (a `protocol` whose `readable` returns canned
  `PROTO_DONE`) for unit testing without a network or scripting runtime.

## Acceptance

- Orchestrator is unit-testable with the stub protocol — no real network, no engine
  (ADR Consequences → Positive).
- **Invariant 1 holds:** `orchestrator.c` `#include`s no protocol/engine header other
  than `proto.h` and `script_api.h`. `grep -r 'lua\.h\|quickjs\.h' src/orchestrator.c`
  is empty.
- `make test` green.
