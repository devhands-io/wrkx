title: Refactored wrkx must report exact rate — measure elapsed before progress-join
status: completed
adr: 0003
depends: t040

## Context

t040 re-vendored the baseline from `505cd14` (pre-progress-bar) so the
behavioural reference reports the requested rate EXACTLY (`-R3000` →
`Requests/sec 3000.13` on an idle box). With an exact reference in place, the
refactored `wrkx` is now visibly ~0.3 % under the requested rate — because it
ported the progress bar and carries the SAME root cause t040 documented for the
post-`bbd3d56` builds: the progress-thread join sits INSIDE the measured window.

t037 (Decision A) fixed the *start* anchor — `run_start = time_us()` is captured
after the thread/event-loop create loop, so setup time is excluded. But it left
the *end* of the window after the progress-thread join:

```c
uint64_t run_start = time_us();
for (...) pthread_join(workers);     // ≈ stop_at + small drain
parg.done = 1;
pthread_join(progress_thread, NULL); // BLOCKS on sleep(1)-granularity wakeup + erase/fflush
uint64_t elapsed = time_us() - run_start;   // charges that block to runtime
```

The progress thread re-checks completion only once per `sleep(1)` and then does
a trailing terminal erase (`printf("\r%60s\r",""); fflush`). Both land inside
`[run_start, elapsed]`, inflating the denominator by a few–tens of ms — ~0.3 %
over a 20 s run, exactly enough to turn `3000.1` into `~2990`.

The parity gate's 25 % rps tolerance keeps CI green, so this is a sub-percent
accuracy fix, not a correctness regression — but with the exact-3000 reference it
is now measurable and worth closing.

## Root cause

Reported rate = `complete / (elapsed_us / 1e6)`. The numerator is correct in
every version; the defect is purely the denominator's right edge. Nothing may be
inserted between the worker join and the `elapsed` reading. The progress-thread
signal + join is inserted exactly there.

## Resolution

Capture `elapsed` immediately after the worker join, BEFORE signalling and
joining the progress thread:

```c
for (...) pthread_join(workers);
uint64_t elapsed = time_us() - run_start;   // window ends with the workers
parg.done = 1;
pthread_join(progress_thread, NULL);          // teardown, outside the window
```

The progress thread's wakeup lag and trailing erase are pure presentation
teardown and must not be charged to the measured run, matching pre-`bbd3d56`
phase-0 (and the t040 baseline) which read `runtime_us` right after the worker
join.

## Acceptance

- `wrkx -R3000` against a static localhost server on an idle box reports
  `Requests/sec` ≈ 3000.x (matching `baseline/wrkx0`), not ~2990.
- `make test` green; `make parity` green (instant + kalimit).
- `scripts/compare.sh instant -- -t4 -c16 -d10s -R3000 -L` shows NEW within a
  fraction of a percent of OLD (well inside tolerance), not a consistent ~0.3 %
  shortfall.
- The progress thread is still signalled and joined (no leak / no truncated
  bar) — only the `elapsed` capture moves ahead of it.
