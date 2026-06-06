title: Redis protocol skeleton — vtable, RESP codec, auth lifecycle
status: completed
adr: 0005
adr-step: P2-1
depends: —

## Goal

Implement the first non-HTTP protocol extension: a Redis client that fits into the
existing `protocol` vtable without modifying `engine.c`, `orchestrator.c`, or any
core scheduling code. This is the **Gate A check** — the architecture's central
protocol-independence claim is confirmed or falsified here.

## Context

ADR 0005 P2-1. Redis is chosen as the cheapest real proof: it is maximally
different from HTTP (binary-ish RESP framing, persistent connection, AUTH handshake)
while remaining protocol-simple (line-based RESP, one command per request at this
stage). If the vtable cannot express Redis without core edits, the ADR is revised
before further investment.

## Deliverables

- `src/proto/redis/` directory:
  - `redis.c` — implements the `protocol` vtable (`proto->connect`, `proto->readable`,
    `proto->writable`, `proto->close`)
  - `resp.c` / `resp.h` — RESP encode/decode (inline command, bulk string, array,
    status/error/integer replies)
- `src/proto/redis/redis.h` — public API (registration function, config struct)
- AUTH + SELECT lifecycle inside `proto->connect()` (connection-level, not
  per-request)
- One command per request (no pipelining; deferred to t052)
- Extension registration skeleton (`proto_redis_register()`)

## Guards / Acceptance

1. **RESP unit tests** — `tests/unit/test_resp.c`:
   - encode inline command, bulk string array
   - decode status reply, error reply, integer reply, bulk string reply
   - partial-read buffering (split across two `readable` calls)
2. **Auth handshake test** — mock server that demands `AUTH password`; connection
   succeeds with correct credential, fails with wrong credential.
3. **Gate A check (diff guard):**
   ```
   git diff main -- src/orchestrator.c src/orchestrator.h \
       src/proto/proto.h src/ae.c src/rate.c | grep -c '^[+-]'
   ```
   Must be zero (or contain only additive changes explicitly accepted in a note
   below this line). If non-zero, stop and revise ADR 0005.
4. `make test` green (unit suite).

## Note

Do not implement pipelining here. It proves a separate, stronger claim (Gate B)
and is tracked in t052. Pipelining mixed into the initial skeleton would conflate
two signals.
