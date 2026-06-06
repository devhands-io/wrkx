title: Redis E2E — dummy RESP server, main.c protocol wiring, Gate A confirmation
status: todo
adr: 0005
adr-step: P2-3
depends: t050

## Goal

Wire Redis into the wrkx entry point (main.c) so that `redis://` URLs select
the Redis protocol stack end-to-end: URL parsing → protocol detection →
redis_configure() → orchestrator → RESP traffic → latency/throughput reporting.

Prove the architecture is correct with a self-contained E2E test that uses a
dummy Python RESP server instead of a real Redis installation. No CI service
container required.

## Context

ADR 0005 P2-3. Protocol vtable, RESP codec, and Lua glue module are complete.
The only missing piece is the wiring layer. The dummy Python server replies
with canned RESP responses (PING→PONG, SET→OK, GET→$5\r\nvalue\r\n) — sufficient
to exercise the full connection→write→readable→stats path without a real Redis.

Using a dummy server (not a real Redis) makes E2E tests self-contained, avoids
CI service containers, and gives deterministic latency for rate-control assertions.

## Deliverables

- **`tests/e2e/redis_mock_server.py`** — threaded RESP server; parses RESP bulk
  arrays, dispatches on command name, replies with canned RESP responses; handles
  pipelining (multiple commands per recv()); exits cleanly on SIGTERM
- **`tests/e2e/redis_basic.sh`** — E2E script (mirrors smoke.sh): starts mock
  server, runs wrkx at -R20 and -R200, asserts Requests/sec within 5% of target
- **`scripts/redis_get_set.lua`** — alternating GET/SET workload script using
  redis.command()
- **`src/main.c`** — protocol detection table (`schema_table[]` + `detect_protocol()`),
  `redis://`/`rediss://` branch for configure+vtable+lua-helpers, default port 6379
- **`Makefile`** — add redis_basic.sh to test-e2e run list

## Protocol detection design

A static `schema_table[]` in main.c maps schema string → (proto_kind, need_tls,
default_port). `detect_protocol(schema)` does a linear scan and returns a
`proto_info` struct. Adding a future protocol is one table row + one enum value +
one `case` in the configure switch — no scattered condition chains.

```
schema_table:
  "http"  / "https"  → PROTO_HTTP,  port 80
  "redis" / "rediss" → PROTO_REDIS, port 6379
```

## wrkx invocation (E2E test)

```
./wrkx -t1 -c5 -d3s -R20  -s scripts/redis_get_set.lua redis://localhost:PORT
./wrkx -t1 -c10 -d3s -R200 -s scripts/redis_get_set.lua redis://localhost:PORT
```

## Guards / Acceptance

1. `make` — wrkx binary builds clean with new main.c includes
2. `make test-unit` — all existing unit tests still green
3. `bash tests/e2e/redis_basic.sh` — passes with dummy server:
   - Requests/sec ≥ 19 at -R20 (within 5%)
   - Requests/sec ≥ 190 at -R200 (within 5%)
   - Transfer/sec > 0
   - exit code 0
4. `make test` — full suite (unit + e2e) green
5. `make gate-a-check` — PASS (only main.c touched in src/)

## Not in scope

- Real Redis integration (t052 performance smoke test may use real Redis)
- Password/db URL extraction is wired but untested end-to-end (no AUTH in mock)
- pipelining (t052)
- --proto override flag
