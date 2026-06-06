title: Redis E2E — real service, CI integration, Gate A confirmation
status: todo
adr: 0005
adr-step: P2-3
depends: t050

## Goal

Run a complete Redis benchmark end-to-end: real Redis server, Lua workload script,
wrkx reporting latency / throughput / errors. Confirm Gate A: the Redis extension
required no changes to the core engine or orchestrator.

## Context

ADR 0005 P2-3. This is the integration proof for Phase 2. The rate control,
coordinated-omission correction, HdrHistogram latency accounting, and report
formatting are all reused unchanged from the HTTP path — the fact that they work
unchanged is itself evidence the architecture is right.

## Deliverables

- **CI service container** — GitHub Actions `services:` block with Redis (latest
  stable), accessible at `localhost:6379`
- **Example workload** — `scripts/redis_get_set.lua`:
  - `init()`: seed keys (`SET key:<i> value:<i>` for i in 1..1000)
  - `request()`: alternating `GET key:<random>` / `SET key:<random> <value>`
  - `response(status, headers, body)`: validate reply type, count errors
- **wrkx invocation** — `./wrkx -t4 -c100 -d10s -R1000 --proto redis
  redis://localhost:6379`
- **Output validation** — latency percentiles, Requests/sec, Transfer/sec,
  non-zero byte count, error count all present and sane
- **Gate A diff guard** (automated, in CI):
  ```yaml
  - name: Gate A — no core engine changes
    run: |
      changed=$(git diff origin/main -- src/orchestrator.c src/orchestrator.h \
          src/proto/proto.h src/ae.c src/rate.c | grep -c '^[+-]' || true)
      if [ "$changed" -gt 0 ]; then
        echo "GATE A FAILED: core engine modified for Redis"
        exit 1
      fi
  ```

## Guards / Acceptance

1. E2E test script `tests/e2e/redis_basic.sh`:
   - starts wrkx against Redis service for 10s at 1000 RPS
   - asserts `Requests/sec` ≥ 950 (allows ±5% from rate target)
   - asserts `Transfer/sec` > 0
   - asserts errors = 0 (or within configured threshold)
   - asserts latency P99 is present and non-zero
2. CI matrix: Linux and macOS both green.
3. **Gate A confirmed:** the CI diff guard passes (zero core-engine changes).
4. `make test` (full suite including unit + e2e) green.

## Performance smoke test

Run at `-R3000` with `-c100 -t4 -d20s`. Requests/sec should be within 1% of 3000
(same accuracy as HTTP baseline, since rate control is shared). If it is not,
investigate before proceeding to t052 — the deficit may indicate a protocol-layer
inefficiency rather than a rate-control issue.
