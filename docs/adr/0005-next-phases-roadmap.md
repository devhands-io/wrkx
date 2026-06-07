# 5. Next-Phases Roadmap

| Field         | Value              |
|---------------|--------------------|
| Status        | Accepted           |
| Date          | 2026-06-06         |
| Phase         | Phases 2–10        |
| Deciders      | wrkx core team     |
| Supersedes    | —                  |
| Superseded by | —                  |

---

## Context

ADR 0001 establishes the three-layer architecture (Orchestrator / Protocol Engine /
Request Layer) and proves the architecture against HTTP/1.1. The roadmap requires
extending wrkx to support:

- Additional network protocols: Redis, memcached, PostgreSQL, MySQL, HTTP/2, gRPC,
  HTTP/3
- A second scripting engine: JavaScript (QuickJS), in addition to LuaJIT
- A formal extension system enabling protocols to be packaged and loaded independently
- Alternate workload sources: HAR replay, CSV/JSON-driven request streams

The question is not *what* to build but *in what order* and *how to structure the
phases* so that each step proves one architectural claim before the next investment
is made.

---

## Decision Drivers

1. **Fail fast, fail cheap.** Each phase proves one architectural gate. If the gate
   fails the plan stops and the ADR is revised — having spent one unit of effort, not
   five.
2. **One claim per phase.** Phases that prove multiple independent claims obscure
   which one failed. Each phase has a named failure condition.
3. **Risk ordering.** The cheapest proof of each claim comes first. Expensive or
   high-risk work comes only after cheaper analogues have validated the foundation.
4. **Extension independence before extension multiplication.** The extension boundary
   must be proven before several protocols depend on it.
5. **Scripting independence before scripting-coupled APIs.** A second scripting engine
   must be proven before protocol helpers accrete Lua-specific coupling.

---

## Decision

### Phase structure and gates

Each phase has a named **gate** — a binary pass/fail criterion. Failure at a gate
means halt-and-revise, not continue.

| Phase | Purpose                        | Gate |
|-------|-------------------------------|------|
| 2     | Redis vertical slice           | A    |
| 3     | Extension system hardening     | C    |
| 4     | memcached (second extension)   | C′   |
| 5     | JavaScript scripting engine    | D    |
| 6     | Stateful database protocols    | E    |
| 7     | Request sources / ammo layer   | F    |
| 8     | HTTP/2                         | G    |
| 9     | gRPC                           | G′   |
| 10    | HTTP/3 / QUIC                  | H    |

### Architectural gates

**Gate A — protocol independence**
Passed by: Redis basic E2E (P2-3).
Failure condition: Redis requires edits to `engine.c` or `orchestrator.c`.

**Gate B — outstanding-operation model**
Passed by: Redis pipelining (P2-4).
Failure condition: latency/accounting/rate logic assumes exactly one in-flight
request per connection.

**Gate C — extension independence**
Passed by: Redis loaded as a real extension (P3-3).
Failure condition: extensions need private core headers or special-case build wiring.

**Gate C′ — extension API not Redis-shaped**
Passed by: memcached implemented as an extension without changes to the extension
API (P4-2).
Failure condition: the extension interface requires modification to accommodate
memcached.

**Gate D — scripting independence**
Passed by: the same Redis workload running in Lua and JavaScript (P5-3 / P5-4).
Failure condition: protocol helpers are coupled to Lua-specific types or lifecycle.

**Gate E — stateful protocol complexity**
Passed by: PostgreSQL extended query protocol (P6-2).
Failure condition: multi-step prepared-statement state leaks into the scheduler or
request layer.

**Gate F — request-source independence**
Passed by: HAR replay (P7-2).
Failure condition: workload generation is script-shaped rather than
request-source-shaped.

**Gate G — multiplexing**
Passed by: HTTP/2 concurrent streams (P8-3).
Failure condition: the engine cannot cleanly model multiple logical requests over one
connection.

**Gate G′ — semantic layering on multiplexed transport**
Passed by: unary gRPC E2E (P9-1).
Failure condition: gRPC framing, metadata, or status handling requires changes to
HTTP/2 transport code.

**Gate H — transport abstraction**
Passed by: basic HTTP/3 E2E (P10-2).
Failure condition: transport assumptions are TCP/TLS-specific and leak into protocol
execution.

---

## Phase 2 — Redis vertical slice

**Claim:** the three-layer engine can run a non-HTTP protocol without modifying the
core engine or orchestrator.

**Why Redis:** maximally different from HTTP (binary-ish, persistent-connection,
auth handshake, pipelining) while remaining protocol-simple (RESP framing is
line-based). This combination isolates the architectural signal: if something breaks,
it is the architecture, not Redis minutiae.

### P2-1 Redis protocol skeleton

Deliverables:
- `src/proto/redis/` — protocol vtable implementation
- connect/auth/SELECT lifecycle inside `proto->connect()`
- basic RESP encode/decode (one command per request, no pipelining)
- extension registration skeleton

Guards:
- RESP encode/decode unit tests
- connection/auth handshake tests
- **Gate A check:** `engine.c` and `orchestrator.c` are diff-clean

### P2-2 Redis Lua scripting bindings

Deliverables:
- `redis.command(cmd, ...)` Lua helper
- Redis-specific request object construction through the Request Layer
- `redis.pipeline(...)` may be stubbed or explicitly deferred

Guards:
- Lua binding unit tests
- invalid-command error tests
- script-to-protocol object tests

### P2-3 Redis E2E

Deliverables:
- CI: real Redis service (Docker / GitHub Actions service container)
- example Lua workload (`scripts/redis_get_set.lua`)
- latency / throughput / error reporting reusing existing machinery unchanged

Guards:
- E2E test against real Redis
- CI integration on both Linux and macOS
- performance smoke test (rate matches `-R` target)
- **Gate A confirmed:** no core engine changes were required

### P2-4 Redis pipelining

**Why split from basic Redis:** basic Redis (P2-1–3) proves "non-HTTP request/response
protocols work." Pipelining proves the stronger claim: "the engine can account for
multiple outstanding logical operations on one physical connection." That is the same
family of problem as HTTP/2 stream multiplexing, but cheaper to prove.

Deliverables:
- pipeline depth configuration (`--pipeline N`)
- response correlation / ordering
- per-command latency attribution
- backpressure behaviour

Guards:
- pipelined Redis E2E
- latency correctness tests (per-command, not per-batch)
- **Gate B confirmed:** no changes to scheduler semantics, or changes explicitly
  accepted with a follow-up ADR

---

## Phase 3 — Extension system hardening

**Claim:** protocols can be packaged as independent extensions without accessing
private core internals.

**Why before more protocols:** Redis might accidentally fit because the extension
boundary was designed around it. A second protocol (memcached, Phase 4) confirms
the boundary is not Redis-shaped. But the boundary must first exist as a formal
interface before memcached is attempted.

### P3-1 Extension API/ABI boundary

Deliverables:
- stable `include/wrkx_extension.h` (protocol registration, request-layer
  registration, scripting-binding registration)
- version / capability negotiation
- documented invariant: "what extensions may not touch"

Guards:
- toy extension compiles and loads against the ABI
- loading-failure tests (wrong version, missing symbol)
- ABI/API compatibility test

### P3-2 Extension packaging and build support

Deliverables:
- static or dynamic loading policy decision
- extension manifest format
- `make extension=NAME` build support
- optional/experimental extension flags
- developer documentation: "How to write a wrkx extension"

Guards:
- extension build on/off CI matrix
- documentation review

### P3-3 Convert Redis to reference extension

Deliverables:
- Redis moved to `extensions/redis/` and built through the extension mechanism
- loaded/disabled via extension manifest, not build-time flag

Guards:
- existing Redis E2E passes unchanged after the move
- **Gate C confirmed:** Redis is a real extension with no private-header access

---

## Phase 4 — memcached (second extension)

**Claim:** the extension API is not Redis-shaped — a second, meaningfully different
data-store protocol can be added without modifying the extension interface.

### P4-1 memcached text protocol

Deliverables:
- `extensions/memcached/` — protocol vtable
- `get`, `set`, `delete`, `incr`, `decr`
- text protocol framing
- connection reuse

Guards:
- parser unit tests
- E2E against real memcached
- no core engine changes

### P4-2 memcached E2E and Gate C′

Deliverables:
- CI: memcached service container
- example Lua workload
- throughput / latency reporting

Guards:
- E2E test
- **Gate C′ confirmed:** extension API required no changes to accommodate memcached

### P4-3 memcached binary protocol (optional / deferrable)

Deliverables:
- binary request/response framing
- opaque/correlation handling
- error/status handling

Guards:
- binary parser tests
- E2E binary-mode workload

*This subphase may be deferred past Phase 5 if binary framing is not needed as a
stepping stone before PostgreSQL.*

---

## Phase 5 — JavaScript scripting engine

**Claim:** the Request Layer abstraction (`script_api.h`) is not Lua-shaped — a
second scripting engine can implement it without changes to the protocol or
orchestrator layers.

**Why before databases:** each new database protocol will need scripting helpers.
If the scripting abstraction has not been proven engine-agnostic before those helpers
are written, they will accrete Lua-specific coupling that is expensive to undo.

### P5-1 Script API conformance cleanup

Deliverables:
- finalize `script_api.h`: explicit lifecycle (`init`, `per-VU setup`, `request`,
  `response`, `teardown`)
- language-neutral request construction
- language-neutral protocol-helper registration
- script engine conformance test suite

Guards:
- existing Lua tests pass unchanged
- conformance suite green against LuaJIT engine

### P5-2 QuickJS integration

Deliverables:
- QuickJS runtime embedding (`deps/quickjs/`)
- module registration matching Lua helper pattern
- JS equivalents of core request APIs
- deterministic cleanup / GC lifecycle
- runtime limits (memory, execution time) if needed

Guards:
- JS unit tests
- memory-leak tests (valgrind / ASAN)
- parity test: simple HTTP workload in JS matches Lua output

### P5-3 JS Redis bindings

Deliverables:
- `redis.command(...)` in JavaScript
- Redis E2E workload in JS
- same workload expressible in both Lua and JS

Guards:
- Lua / JS parity test for Redis workload
- no Redis protocol-layer changes required

### P5-4 Scripting independence confirmation

Guards:
- **Gate D confirmed:** one protocol (Redis), two scripting engines, same core engine,
  same output

### Gate D result — 2026-06-07

**Engines:** LuaJIT (built-in) · QuickJS v0.10.0 (vendored, `deps/quickjs/`)  
**Baseline tag:** `p5-baseline` → commit `0502931` (chore: remove t070 from active tasks)  
**Workload:** `scripts/redis_get_set.{lua,js}` — deterministic GET/SET alternation
across 100 keys (`-t1 -c1 -d2s -R20`, 41 commands each run)

```
PASS  frozen core+protocol unchanged since p5-baseline
PASS  LuaJIT build
PASS  QuickJS build
PASS  Lua/JS Redis request parity

Gate D: 4 passed, 0 failed
```

**Frozen paths verified clean** (no diff from `p5-baseline` to HEAD):
`src/orchestrator.*`, `src/ae.*`, `src/rate.*`, `src/net.*`, `src/transport.*`,
`include/wrkx_extension.h`, `include/wrkx_transport.h`,
`extensions/redis/redis.{c,h}`

**Conclusion:** The `script_api` vtable is not Lua-shaped. The QuickJS engine
implements it (create/configure/capabilities/register\_helpers/clone/init/
request/response/destroy) without a single line of change to the protocol layer,
orchestrator, or event loop. Gate D is confirmed.

---

## Phase 6 — Stateful database protocols

**Claim:** complex, multi-step, stateful protocol lifecycles (auth handshake,
prepared-statement Parse/Bind/Execute/Sync) can be expressed through the protocol
vtable without leaking state into the scheduler or request layer.

**Why PostgreSQL before MySQL:** PostgreSQL's extended query protocol (Parse → Bind →
Describe → Execute → Sync) is a harder stressor than MySQL's prepared statements.
If PostgreSQL works, MySQL is incremental.

**Why split simple query from extended query:** simple query proves connection/auth/
result-parsing works; extended query proves multi-step protocol state works. Those
are different risks and should be separate gates.

### P6-1 PostgreSQL simple query

Deliverables: startup/auth handshake, simple query flow, text result parsing,
error handling, connection teardown, Lua helper (`pg.query(...)`).
Guards: parser tests, auth tests, E2E `SELECT 1`, basic throughput benchmark.

### P6-2 PostgreSQL extended query / prepared statements

Deliverables: Parse, Bind, Describe, Execute, Sync; prepared-statement lifecycle;
parameter binding; result metadata; Lua helper (`pg.prepare(...)`).
Guards: prepared-statement tests, multi-step script tests, E2E prepared workload.
**Gate E confirmed** if passing.

### P6-3 PostgreSQL session features

Deliverables: transaction-oriented workloads, connection reset strategy, SSL/TLS
policy (if in scope), optional statement cache.
Guards: transaction E2E, reconnect tests, failure-mode tests.

### P6-4 MySQL simple query

Deliverables: handshake/auth, simple query, resultset parsing, error packets,
connection lifecycle, Lua helper (`mysql.query(...)`).
Guards: parser tests, E2E `SELECT 1`, basic benchmark.

### P6-5 MySQL prepared statements

Deliverables: `COM_STMT_PREPARE`, `COM_STMT_EXECUTE`, parameter binding,
statement close/reset, Lua and JS binding parity.
Guards: prepared-statement E2E, Lua/JS parity test.

---

## Phase 7 — Request sources / ammo layer

**Claim:** workload generation is request-source-shaped, not script-shaped — the
Tank "ammo" concept is real and the Request Layer can be driven by sources other
than live scripts.

### P7-1 Request-source abstraction

Deliverables: formal request-source interface (`src/ammo/ammo.h`); script source,
static-list source, replay-source implementations; common pacing / rate-control
integration.
Guards: fake-source tests, source-selection tests, Lua still works unchanged.

### P7-2 HTTP HAR replay

Deliverables: HAR parser, HTTP/1.1 request reconstruction, header/body handling,
basic replay controls (loop, stop-after-N).
Guards: HAR fixture tests, HTTP E2E replay test.
**Gate F confirmed** if passing.

### P7-3 Generic data-driven workloads

Deliverables: CSV/JSON request source; parameterized script workloads; Redis /
memcached / PostgreSQL command-stream replay examples.
Guards: Redis replay-like workload, PostgreSQL query-list workload.

---

## Phase 8 — HTTP/2

**Claim:** the engine can model multiple logical requests over one physical connection
(stream multiplexing) without requiring structural changes to the orchestrator.

**Why last among "simple" protocols:** HTTP/2's multiplexing is the one feature most
likely to challenge the core engine assumption (one in-flight operation per connection)
shared by HTTP/1.1, Redis, and PostgreSQL. Confronting it after all other extensions
have validated that assumption means a forced revision is isolated and clearly
attributed.

### P8-1 Engine concurrency model review

A design checkpoint, not implementation. Deliverables: documented model for
connection / stream / logical-request / response / in-flight operation; explicit
decision on whether the protocol vtable needs stream-level callbacks; ADR update
if the model changes.
Guards: checklist of core assumptions; proof that Redis pipelining and HTTP/2
multiplexing use the same or intentionally different abstraction.

### P8-2 Basic HTTP/2 transport

Deliverables: TLS/ALPN, connection preface, SETTINGS, stream creation, basic GET/POST,
HPACK integration (one request at a time initially).
Guards: frame encode/decode unit tests, E2E against known HTTP/2 server.

### P8-3 HTTP/2 multiplexing

Deliverables: concurrent streams, stream-ID tracking, response correlation,
per-stream latency attribution, flow-control, max-concurrent-streams behaviour.
Guards: multiplexed E2E, latency attribution tests, backpressure tests.
**Gate G confirmed** if passing.

### P8-4 HTTP/2 scripting API

Deliverables: HTTP/2 options exposed to Lua and JS (stream count, push handling),
example workloads.
Guards: Lua/JS parity tests, HTTP/1.1 workloads unchanged.

---

## Phase 9 — gRPC

**Claim:** gRPC (semantic/application-layer framing on top of HTTP/2) can be
implemented without changes to the HTTP/2 transport layer.

### P9-1 Unary gRPC

Deliverables: protobuf payload support strategy, service/method naming, metadata,
unary request/response, status/trailer handling.
Guards: E2E against test gRPC service, invalid-status tests, latency attribution.
**Gate G′ confirmed** if passing.

### P9-2 gRPC scripting helpers

Deliverables: Lua and JS helper APIs, descriptor/proto loading strategy (if in scope),
binary payload handling, example workloads.
Guards: Lua/JS parity tests.

### P9-3 Streaming gRPC (optional / later)

Deliverables: server streaming, client streaming, bidirectional streaming, stream
lifecycle metrics, per-message vs. per-RPC metric decision.
Guards: long-lived stream tests, cancellation tests.

*May be deferred to a separate milestone; streaming gRPC is closer to a distinct
workload model than a conventional request/response protocol.*

---

## Phase 10 — HTTP/3 / QUIC

**Claim:** the transport abstraction is not TCP/TLS-specific — QUIC transport can
be substituted without changes to HTTP/3 protocol-layer code.

**Why last:** HTTP/3 combines QUIC transport, TLS 1.3 integration, stream
multiplexing, and connection-migration semantics. By Phase 10, the stream/request
model will have been clarified by HTTP/2, and the transport abstraction will have
been exercised by TCP, TLS, and QUIC in ascending complexity.

### P10-1 QUIC transport spike

Deliverables: QUIC library selection, basic connection, TLS configuration, ALPN,
connection lifecycle.
Guards: connect/disconnect tests, failure-mode tests.

### P10-2 Basic HTTP/3

Deliverables: HTTP/3 request/response, QPACK integration, GET/POST.
Guards: E2E against HTTP/3 server, frame/header tests where practical.
**Gate H confirmed** if passing.

### P10-3 HTTP/3 concurrency and metrics

Deliverables: concurrent streams, per-stream latency, connection-level metrics,
error/cancellation handling.
Guards: multiplexed E2E, latency attribution, comparison with HTTP/2 behaviour.

---

## Consequences

### Positive

- Each phase has a named failure condition; a failed gate stops the plan before the
  next investment.
- Extension independence (Phase 3) is proven before multiple extensions exist, so
  retrofitting is never necessary.
- Scripting independence (Phase 5) is proven before database-specific helpers are
  written, preventing Lua-shaped APIs.
- HTTP/2 multiplexing risk (Gate G) is isolated at Phase 8, after every other
  extension has validated the one-in-flight assumption.

### Negative / trade-offs

- Ten phases is a long roadmap. Phases are independent in Phase 3 and beyond (once
  Phase 2 gates pass), so parallelism is possible with multiple contributors.
- The extension API (Phase 3) is designed before seeing memcached (Phase 4). Some
  iteration on the API boundary after Phase 4 is expected and acceptable.
- PostgreSQL (Phase 6) is the first database protocol only after JavaScript (Phase 5)
  is proven. Teams that need database support sooner may find this ordering
  frustrating, but the alternative risks Lua-coupled database helpers.

### Note on task numbering

Tasks implementing this ADR use `adr: 0005` and `adr-step: PX-Y` in their headers.
Phase 2 tasks begin at `t049`.
