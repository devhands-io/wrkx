title: Baseline must report exact rate — re-vendor from pre-progress-bar wrk2
status: completed
adr: 0003
depends: t039

## Context

The frozen baseline (`baseline/wrkx0`, then commit `ea8ea9e`) and the refactored
`wrkx` both reported `Requests/sec` a hair UNDER the requested rate (e.g. 2990
for `-R3000`), while the original upstream `wrk` reported it EXACTLY (3000.13).
Since the baseline is the behavioural reference for old-vs-new comparison, the
reference itself must be exact — otherwise "new is slightly low" cannot be told
apart from "the reference is slightly low."

## Root cause — progress-thread join inside the measured window

Reported rate = `complete / (runtime_us / 1e6)`. The numerator (~60023 for
`-R3000 -d20`) is correct in every version. The defect is purely in the
denominator `runtime_us` — what wall-clock window it spans.

Open-model pacing makes the workers issue ~`rate × duration` requests over
`[T_start, stop_at]`, where:

```
T0      = time_us()              stop_at = T0 + duration   (workers stop here)
        ... create threads ...
T_start = time_us()              // runtime clock starts
        ... workers run to stop_at, then drain ...
        pthread_join(workers)    // ≈ stop_at + small drain
        runtime_us = time_us() - T_start
```

For the rate to come out exact, nothing may be inserted between the worker join
and the `runtime_us` reading. The progress-bar commit `bbd3d56` inserted exactly
that:

```
        pthread_join(workers)            // ≈ stop_at
        g_progress_done = 1              // signal the progress thread
        pthread_join(progress_thread)    // BLOCKS until it exits
        runtime_us = time_us() - T_start // now includes that block
```

The progress thread's run-bar loop re-checks completion only once per
`sleep(1)`:

```c
while (!g_progress_done) {
    ... draw bar; if (pct >= 1.0) break;
    sleep(1);                         /* 1-second granularity */
}
printf("\r%60s\r", ""); fflush(stdout);  /* erase line — terminal I/O */
```

Two costs now land inside `[T_start, runtime_us]` that were never there before:

1. **Sleep-granularity wakeup lag.** When workers stop at `stop_at`, the progress
   thread is usually mid-`sleep(1)` and its phase rarely aligns with `stop_at`,
   so it finishes the run a few–tens of ms late. The main thread is already
   blocked in `pthread_join(progress_thread)`, so that lag is charged to
   `runtime_us`.
2. **The trailing erase + `fflush`** after the loop (a terminal write), also
   inside the window.

The inflation is small and variable (tens of ms) but over a 20 s run it is
~0.3 % — exactly enough to turn `3000.13` into `~2990`. The pre-`bbd3d56`
original has no progress thread, so `runtime_us` is read immediately after the
worker join → exact rate.

## Resolution (baseline)

Re-vendor `baseline/src/` from commit `505cd14` — the last commit before the
progress bar (`bbd3d56`) — whose `runtime_us` is measured immediately after the
worker join (pristine wrk2 behaviour). Verified on an idle box:
`-R3000` → `Requests/sec 3000.13`, matching the original `wrk`.

Not the literal root `5daf8ed`: it targets LuaJIT 2.0 (`struct luaL_reg`) and
lacks the non-Linux `cpu_set_t`/affinity guards, so it will not build against
this repo's LuaJIT 2.1 on macOS without editing the frozen source. `505cd14` is
past those platform fixes (`f3e2b56`) yet behaviourally identical to root for
rate (pacing + runtime-measurement code is unchanged root..505cd14).

Done in commit `68486ac`:
- `baseline/src/` re-vendored from `505cd14` (binary self-reports `wrkx 4.0.0`,
  no progress thread).
- `baseline/MANIFEST.sha256` regenerated; `baseline/verify.sh` green.
- `baseline/README.md`, `Makefile` provenance + rationale updated.

## Acceptance (met)

- `baseline/wrkx0 -R3000` reports `Requests/sec` ≈ 3000.x on an idle box
  (matches upstream `wrk`), not ~2990.
- `make baseline-verify` OK (31 files); `make test` and `make parity` green.

## Follow-up (out of scope — separate task if desired)

The refactored `wrkx` still ported the progress bar, so it carries the SAME
root cause: the progress-thread join sits inside its measured window (t037 moved
the start anchor after thread creation but left `elapsed = time_us() - run_start`
*after* `pthread_join(progress_thread)`). To make the new binary also report
exact rate, read `elapsed` right after the worker join, before signalling/joining
the progress thread. The parity gate's tolerance keeps this sub-percent residual
from failing CI, so it is left as a deliberate follow-up.

Note: ADR 0003's Context references the prior `ea8ea9e` baseline and its
measured numbers; that text is historical (the ADR is Accepted/immutable) and is
not edited here.
