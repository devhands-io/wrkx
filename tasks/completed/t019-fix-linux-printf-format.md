title: Fix Linux CI — printf format specifiers for int64_t/uint64_t
status: completed
depends: t17

Context:
- On Linux, uint64_t is `unsigned long` and int64_t is `long`.
- On macOS, both are `unsigned long long` / `long long`.
- The debug printf block in response_complete (wrk.c ~line 620) uses %lld
  for all values, which GCC rejects as -Werror=format on Linux.
- The portable fix is PRId64 / PRIu64 from <inttypes.h> (already included).

Affected lines in src/wrk.c (response_complete debug block):
  expected_latency_timing  — int64_t  → PRId64
  now                      — uint64_t → PRIu64
  expected_latency_start   — uint64_t → PRIu64 (appears twice)
  c->thread_start          — uint64_t → PRIu64
  c->complete              — uint64_t → PRIu64
  c->latest_should_send_time — uint64_t → PRIu64
  c->latest_expected_start — uint64_t → PRIu64
  c->latest_connect        — uint64_t → PRIu64
  c->latest_write          — uint64_t → PRIu64

Steps:
- Replace every %lld in the debug block with the correct PRI macro
- Run `make clean && make && make test` locally (macOS)
- CI Linux job should go green on the next push

Acceptance:
- `make 2>&1 | grep -c warning` == 0 on macOS
- Linux CI job passes (no -Werror=format errors)
