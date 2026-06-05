title: Fix reported Requests/sec — measure elapsed from after thread creation
status: todo
adr: 0003
adr-step: A
depends: t032

## Context

ADR 0003 Defect A. The new `wrkx` under-reports Requests/sec (e.g. 2989.64 for
`-R3000` where phase-0 reports 3000.17 on the same idle box). The request count
is correct; the **denominator is wrong**. `orchestrator_run` uses a single
`o->start_us` (captured before worker-thread creation) as both the `stop_at`
anchor and the elapsed base, so reported elapsed includes thread/event-loop
setup time. Phase-0 `wrk.c` captures the runtime clock *after* the creation loop
(`wrk.c`'s `start = time_us()` at the post-loop point) so its denominator
excludes setup. See ADR 0003 §"Decision A".

## Scope

- `src/orchestrator.c` only. No public signature changes.

## Steps

1. In `orchestrator_run`, keep `o->start_us = time_us()` as the **stop_at**
   anchor (threads still stop at a fixed wall-clock duration).
2. After the worker `pthread_create` loop and before the join loop, capture:
   ```c
   uint64_t run_start = time_us();
   ```
3. Compute `elapsed = time_us() - run_start` and set
   `o->result.start_us = run_start; o->result.elapsed_us = elapsed;`.
   Use `run_start`/`elapsed` for the `Requests/sec` and `Transfer/sec` math in
   the report stage.
4. Leave the progress thread, drain, and per-thread pacing (`rate.thread_start`)
   unchanged — only the *reported* measurement window moves.

## Acceptance

- On an idle box against a static keep-alive server, `wrkx -R<N>` reports
  Requests/sec ≈ N (matching phase-0), not N×(elapsed_with_setup/duration).
- `baseline/wrkx0` vs `wrkx` via `scripts/compare.sh` agree on Requests/sec
  within tolerance for `instant`/`keep-alive` modes.
- All existing unit + E2E tests stay green (`make test`).
- `baseline-verify` still passes (no change to frozen code).
