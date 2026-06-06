title: Replicate baseline rate exactly — restore drain + anchor run_start to load-start
status: completed
adr: 0003
depends: t043

## Context

After t043 the refactored `wrkx` reported `-R3000` at ~2999.9 — closer to 3000
but still consistently *under*, while `baseline/wrkx0` reads ≥3000. The user
asked for it to read 3000+ exactly like baseline. This task supersedes t043's
approach (its prompt-stop is reverted) in favour of replicating baseline's
measurement semantics faithfully.

## Why t043 alone couldn't reach ≥3000

t043 stopped each worker's loop *at* stop_at (no drain). That yields the
cleanest denominator, but it also drops the post-stop_at completions baseline
counts, so the request count can never exceed `rate × duration`:

- prompt-stop: complete ≈ 59996 (a hair under 60000 — open-model pacing anchors
  each connection's clock at *connect* time, a few hundred µs after the run's
  start_us, so each connection paces over slightly less than the full window).
  runtime ≈ 19.9996s → ~2999.9, structurally just below 3000.

Baseline reads ≥3000 because of TWO things working together, BOTH of which t043
removed or never had:

1. **The drain.** Baseline lets the loop run ~one send-interval past stop_at; the
   final scheduled sends complete and are counted, so complete ≈ 60022 (just
   *over* 60000).
2. **A later measurement origin.** Baseline's create loop runs `script_create()`
   (a Lua state) per thread, so by the time it captures `start` the workers have
   already spun up (~1.5–2.5ms later than pthread_create returning). That makes
   its window marginally shorter. The refactor's shared-engine setup is leaner,
   so wrkx captured run_start ~1.5ms too early — lengthening runtime and
   under-reporting.

## Decision (chosen by user: "replicate baseline exactly")

1. **Revert t043's stop timer.** Restore the natural drain so trailing
   completions are counted exactly as baseline does (complete ≈ 60022).
2. **Anchor run_start to actual load-start via a worker-ready barrier.** Each
   worker does `__sync_fetch_and_add(&o->workers_ready, 1)` after setup, right
   before `aeMain`. orchestrator_run spins (`sched_yield`) until
   `workers_ready == n_threads`, then captures run_start. `pthread_create`
   returning does not mean the thread is running; this makes the measured window
   begin when every worker is actually in its loop with connects scheduled —
   the same effect baseline gets implicitly from its slower create loop.

This is a genuine correctness improvement to the measurement origin, not a fudge
factor: it removes the systematic ~1.5ms under-exclusion of spin-up time.

## Implementation

- `struct orchestrator`: add `volatile int workers_ready`.
- `thread_main`: `__sync_fetch_and_add(&o->workers_ready, 1)` before `aeMain`.
- `orchestrator_run`: spin on `workers_ready < n_threads` (`sched_yield`) before
  `run_start = time_us()`.
- Remove `stop_event()` and its per-worker stop timer (t043).
- `#include <sched.h>`.

## Acceptance (met)

- `wrkx -c28 -t27 -d20 -R3000 http://localhost` now tracks `baseline/wrkx0`
  run-for-run and straddles 3000 (measured: NEW 2999.90 / 2999.85 / 3000.04 /
  3000.02 vs OLD 3000.00 / 2999.88 / 3000.08 / —). Request count back to ~60022,
  matching baseline. The previous systematic below-3000 bias is gone.
- Like baseline, the exact reading is noise-dependent on a busy box (baseline is
  not always >3000 either); the point is wrkx and baseline now read the same.
- `make test` and `make parity` green.

## Note

t043's analysis of the ~10ms drain remains accurate; we simply choose to KEEP
that drain (count the trailing completions) to match baseline's reported number,
rather than excise it. The barrier is what closes the residual gap.
