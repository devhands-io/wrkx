Question:

I plan to extend the tool and introduce:
- new layered architecture which splits the primitive all-in-one http-focused engine into three layers similar to Yandex.Tank
- grps, http/2, http/3 support
- PostgreSQL, MySQL, memcached, Redis support, which is done by extensions
- Javascript support in addition to Lua

Considering the architecture phase is done review this phases and suggest a more granular split to cover all I mentioned. Maybe change order. 

The two phases play fundamentally different roles, and that difference is the reason for the split. 

Phase 2 is a gate, not a feature

Everything in ADR-001 — "the engine is protocol-agnostic," "adding a protocol means only implementing the vtable," "the scripting layer can be made protocol-aware without touching the engine" — is a claim. Phase 1 builds the architecture that makes the claim plausible, but it doesn't prove it, because Phase 1 only ever runs HTTP. An architecture that's only ever been exercised by the protocol it was extracted from hasn't been tested at all; it's just been rearranged.

Phase 2 exists to falsify or confirm that claim with the cheapest possible real example. It's a go/no-go checkpoint before you invest in four more extensions. If implementing Redis turns out to require editing engine.c or orchestrator.c, the architecture failed its central promise, and you stop and revise ADR-001 — having spent one protocol's worth of effort to discover it, not five. This is the walking-skeleton / tracer-bullet pattern: prove the full vertical slice end to end before scaling horizontally.

Why Redis, and why exactly one. Redis is the maximally different protocol from HTTP that's still simple. That combination is deliberate. The difference is what makes it a real test — it's binary-ish, persistent-connection, has an auth handshake, supports pipelining, none of which look like HTTP. The simplicity is what keeps the test clean: RESP framing is line-based and trivial, so if something breaks during Phase 2, you know it's the architecture, not Redis minutiae. A more complex protocol would conflate "does the architecture generalize" with "can we get the protocol details right," and you'd lose the clean signal. Only one protocol for the same reason — the deliverable of Phase 2 is a binary yes/no on the architecture bet, and three protocols would muddy that answer.

Redis also happens to exercise the specific design decisions from our earlier discussion: AUTH-in-connect proves the "auth handshake lives inside connect()" choice, and pipelining proves the request/response model survives batching.
Why the order P2-1 → P2-2 → P2-3. This is strict dependency, not preference. P2-1 (the protocol) must exist before P2-2 (the scripting helpers) because the glue module physically calls the protocol's C API — you can't register redis.command() against a proto/redis.c that doesn't exist. And P2-1 is itself the real architecture test: if the vtable can't express Redis, you halt here. P2-2 must precede P2-3 (E2E) because the end-to-end test needs a script to drive commands — test_redis.sh can't be written until redis.command() exists to write its fixture. P2-3 comes last because it's the only integration proof: real Redis, real latency, real throughput, validating the whole stack rather than units in isolation.

The guards mirror the layer each task proves: unit encode/decode for the protocol in isolation, scripting tests for the glue and registry, the full E2E suite for integration. A quiet bonus of P2-3 is that the entire calibration/rate/latency/CO machinery from Phases 0–1 is protocol-agnostic, so Redis inherits all of it for free — and the fact that it does inherit it for free is itself evidence the architecture is right.
Phase 3 is additive, parallel, and risk-ordered
Once Phase 2 proves the architecture, Phase 3 is no longer a bet — it's execution. The defining property of Phase 3 is that each task depends on a different Phase 1 layer, which means the tasks are independent of each other and can run in parallel, in any order. That independence is the entire payoff of the layered design, and the dependency column is where you can read it.

P3-1, JavaScript (depends on P1-4, the Request Layer). This is to the scripting layer what Redis was to the protocol layer: a second implementation that proves the abstraction is real and not accidentally Lua-shaped. It implements script_api.h and touches nothing in the protocol or orchestrator layers — hence the dependency on P1-4 alone. QuickJS is chosen for the same reason LuaJIT fits: it's tiny (~210KB), embeddable, no JIT machinery to fight. It sits in Phase 3 rather than Phase 2 because Lua already works through script_api.h, so the scripting abstraction is already partially proven; the riskier, more novel claim was protocol-agnosticism, which is why a second protocol earns the Phase 2 slot and a second scripting engine does not.

P3-2, PostgreSQL (depends on P1-5, the fully wired engine). Databases were a primary roadmap goal, and PgSQL is the hard protocol — stateful always-on auth, binary framing, and the two-phase prepared-statement lifecycle (Parse → Bind → Execute). If Redis proved "simple non-HTTP works," PgSQL proves "complex stateful protocol works." It deliberately follows Redis: you attempt the expensive real-world payoff only after the cheap proof has validated the foundation. Starting with PgSQL would have meant that if you hit a wall, you couldn't tell whether the architecture was wrong or PgSQL was just hard. It depends on P1-5 rather than just P1-3 because prepared statements need the scripting layer to express multi-step sequences, so it leans on the full wired stack including the session manager.

P3-3, log replay (depends on P1-4). This is the roadmap's "play recorded logs," and architecturally it's the Tank "ammo" concept made literal — an alternative source of "what to send next," which is precisely the Request Layer's job. A HAR or pcap parser produces a request stream that feeds the same init/request/response/done hook contract a script would. It depends on P1-4 because it's a Request Layer implementation, and it's pure additive value rather than an architecture proof, which is why it's in Phase 3. Starting with HAR is natural since HAR is HTTP and HTTP is already the proven protocol.

P3-4, HTTP/2 + gRPC (depends on P1-3, the Protocol Engine) — deliberately last. These are grouped and sequenced because gRPC rides on HTTP/2, so HTTP/2 must come first. More importantly, HTTP/2 is the one item in the entire plan most likely to challenge the architecture: its multiplexing breaks the implicit "one in-flight request per connection" assumption that HTTP/1.1, Redis, and PgSQL all share. If anything forces a change to the engine — the single place ADR-001 swore would never change — it's HTTP/2 stream multiplexing. That's exactly why it's last. You confront the protocol most likely to break the core assumption only after every other extension has validated that assumption, so the risk is isolated at the end where it can't poison the rest of the work.
So the order within Phase 3 isn't dependency-forced the way Phase 2 was — the tasks are independent. It's risk-ordered: P3-1 is the low-risk proven pattern, P3-2 is medium (complex but still request/response), P3-3 is orthogonal value, and P3-4 is the high-risk one held back until last. If you had unlimited hands, you could run all four at once; with limited hands, you do them cheapest-and-safest first so that the riskiest one, if it forces an architecture revision, does so after you've extracted maximum proof that the architecture otherwise holds.

Answer:

Your current split is directionally right: Phase 2 should remain a gate, and Phase 3 should remain expansion. The issue is that Phase 3 is too coarse and mixes several different kinds of risk:

1. protocol abstraction risk,
2. scripting abstraction risk,
3. connection/session-model risk,
4. multiplexing/concurrency-model risk,
5. extension-packaging risk,
6. user-facing workload-authoring risk.

I would split the roadmap more granularly so each phase proves one architectural claim.

Recommended phase structure

Phase 2 — First non-HTTP vertical slice: Redis

Keep Redis as the first gate, but make the phase slightly more explicit.

Goal: prove that the new engine can run a non-HTTP protocol without modifying the core engine or orchestrator.

P2-1: Redis protocol extension skeleton

Deliverables:

* proto/redis/
* Redis extension registration
* protocol vtable implementation
* connect/auth/select lifecycle
* basic RESP encode/decode
* one command per request
* no pipelining yet

Guards:

* Redis encode/decode unit tests
* connection/auth tests
* proof that engine.c, orchestrator.c, and core scheduling code are unchanged

This should be the actual architecture gate. If Redis requires core engine edits, stop here.

P2-2: Redis scripting bindings

Deliverables:

* redis.command(...)
* redis.pipeline(...) may be stubbed or deferred
* Lua binding only at this point
* Redis-specific request object creation through the Request Layer

Guards:

* Lua binding tests
* invalid command tests
* script-to-protocol object tests

P2-3: Redis E2E

Deliverables:

* real Redis container/test service
* example workload
* latency/throughput reporting
* rate limiting and coordinated omission machinery reused unchanged

Guards:

* E2E test against Redis
* CI integration
* performance smoke test

P2-4: Redis pipelining

I would split pipelining out instead of including it in the first Redis proof.

Reason: basic Redis proves protocol independence; pipelining proves a stronger claim: the engine can support multiple outstanding logical requests on one physical connection without becoming HTTP-shaped.

Deliverables:

* pipeline depth configuration
* response correlation/order handling
* latency attribution per command
* backpressure behavior

Guards:

* pipelined Redis E2E
* latency correctness tests
* no changes to scheduler semantics unless explicitly accepted

This becomes the bridge toward HTTP/2 multiplexing later.

⸻

Phase 3 — Extension system hardening

Before adding more protocols, prove that extensions are actually extensions, not just directories inside the core tree.

P3-1: Extension ABI/API boundary

Deliverables:

* stable extension interface
* protocol registration API
* request-layer registration API
* scripting binding registration API
* version/capability negotiation
* clear distinction between built-in and external extensions

Guards:

* sample toy extension
* ABI/API compatibility test
* extension loading failure tests
* documented “what extensions may not touch”

This phase should come before PostgreSQL/MySQL/memcached because otherwise each new protocol may accidentally depend on private internals.

P3-2: Extension packaging and discovery

Deliverables:

* static or dynamic extension loading policy
* extension manifest
* build system support
* optional/experimental extension flags
* documentation for writing extensions

Guards:

* build Redis as an extension
* load/disable Redis through the extension mechanism
* CI matrix with extension on/off

This is important because your roadmap explicitly says PostgreSQL, MySQL, memcached, and Redis are done by extensions. Redis should become the reference extension before you add four more.

⸻

Phase 4 — Second simple non-HTTP protocol: memcached

I would do memcached before PostgreSQL.

Your current plan jumps from Redis to PostgreSQL, but memcached gives you a cheap second data-store protocol with different enough behavior to harden the extension API without bringing database complexity too early.

P4-1: memcached text protocol

Deliverables:

* memcached.get
* memcached.set
* memcached.delete
* basic text protocol support
* connection reuse

Guards:

* parser tests
* E2E against real memcached
* no core engine changes

P4-2: memcached binary protocol, optional

This can be deferred, but it is a useful test if you want to validate binary framing before PostgreSQL/MySQL.

Deliverables:

* binary request/response framing
* opaque/correlation handling
* error/status handling

Guards:

* binary parser tests
* E2E binary-mode workload

This gives you a low-cost stepping stone before PostgreSQL’s more complex binary and stateful lifecycle.

⸻

Phase 5 — JavaScript scripting engine

I would move JavaScript earlier than PostgreSQL, but after Redis and extension hardening.

Reason: JavaScript is not just a feature. It validates that the Request Layer is not Lua-shaped. You want that abstraction proven before you write many protocol-specific helpers.

P5-1: Script engine abstraction cleanup

Deliverables:

* finalize script_api.h
* explicit lifecycle hooks:
    * init
    * per-VU setup
    * request generation
    * response handling
    * teardown
* language-neutral request construction
* language-neutral protocol helper registration

Guards:

* existing Lua tests pass unchanged
* script engine conformance test suite

P5-2: QuickJS integration

Deliverables:

* JavaScript runtime embedding
* module registration
* JS equivalents of core request APIs
* deterministic cleanup
* runtime limits if needed

Guards:

* JS unit tests
* memory leak tests
* parity tests with simple Lua workloads

P5-3: JS Redis bindings

Deliverables:

* redis.command(...) in JS
* Redis E2E workload in JS
* same Redis workload expressible in Lua and JS

Guards:

* Lua/JS parity test
* no Redis protocol-layer changes

This is a strong proof: one protocol, two scripting engines, same core engine.

⸻

Phase 6 — Stateful database protocols

Now add PostgreSQL and MySQL. I would split PostgreSQL into smaller pieces and avoid starting with prepared statements.

P6-1: PostgreSQL simple query protocol

Deliverables:

* startup/auth handshake
* simple query flow
* text result parsing
* error handling
* connection teardown

Guards:

* parser tests
* auth handshake tests
* E2E SELECT 1
* basic throughput benchmark

This proves that PostgreSQL can fit without immediately taking on prepared statements.

P6-2: PostgreSQL extended query protocol

Deliverables:

* Parse
* Bind
* Describe
* Execute
* Sync
* prepared statement lifecycle
* parameter binding
* result metadata

Guards:

* prepared-statement tests
* multi-step script tests
* E2E prepared workload

P6-3: PostgreSQL connection/session features

Deliverables:

* startup parameters
* SSL/TLS policy, if in scope
* transaction-oriented workloads
* connection reset strategy
* optional statement cache

Guards:

* transaction E2E
* reconnect tests
* failure-mode tests

P6-4: MySQL basic protocol

Deliverables:

* handshake/auth
* simple query
* resultset parsing
* error packets
* connection lifecycle

Guards:

* parser tests
* E2E SELECT 1
* basic benchmark

P6-5: MySQL prepared statements

Deliverables:

* COM_STMT_PREPARE
* COM_STMT_EXECUTE
* parameter binding
* statement close/reset

Guards:

* prepared-statement E2E
* Lua and JS binding parity

PostgreSQL before MySQL is reasonable because PostgreSQL is likely the more useful architecture stressor. But I would not combine them in one task.

⸻

Phase 7 — Workload sources / ammo layer

Your current plan mentions log replay in Phase 3. I would move it after at least Redis + JavaScript + extension hardening, but before HTTP/2/3.

Reason: log replay is not just a feature. It proves the Tank-like separation between “where requests come from” and “how protocols execute them.”

P7-1: Request source abstraction

Deliverables:

* script source
* static list source
* replay source interface
* common request stream contract
* common pacing behavior

Guards:

* fake request source tests
* source selection tests
* Lua still works through the same interface

P7-2: HTTP HAR replay

Deliverables:

* HAR parser
* HTTP/1.1 request reconstruction
* header/body handling
* basic replay controls

Guards:

* HAR fixture tests
* HTTP E2E replay test

P7-3: Generic data-driven workloads

Deliverables:

* CSV/JSON request source
* parameterized script workloads
* database workload fixtures
* Redis/memcached command streams

Guards:

* Redis replay-like workload
* PostgreSQL query-list workload

This phase makes the “ammo” concept real instead of just script-based load generation.

⸻

Phase 8 — HTTP/2 foundation

I would not group HTTP/2 and gRPC as a single task. HTTP/2 should be its own architectural stress test.

P8-1: Engine concurrency model review

Before implementing HTTP/2, explicitly audit assumptions.

Deliverables:

* documented model for:
    * connection
    * stream
    * logical request
    * response
    * in-flight operation
* decision on whether the protocol vtable needs stream-level callbacks
* ADR update if needed

Guards:

* explicit checklist of core assumptions
* proof that Redis pipelining and HTTP/2 multiplexing use the same or intentionally different abstraction

This is a design checkpoint, not implementation.

P8-2: HTTP/2 client transport

Deliverables:

* TLS/ALPN
* HTTP/2 connection preface
* SETTINGS
* stream creation
* basic GET/POST
* HPACK integration

Guards:

* unit tests around frame encode/decode
* E2E against known HTTP/2 server
* one request at a time initially

P8-3: HTTP/2 multiplexing

Deliverables:

* concurrent streams
* stream ID tracking
* response correlation
* per-stream latency
* flow-control handling
* max concurrent streams behavior

Guards:

* multiplexed E2E
* latency attribution tests
* backpressure tests

This is the real HTTP/2 architecture gate, not basic HTTP/2.

P8-4: HTTP/2 scripting API

Deliverables:

* HTTP/2 options exposed to Lua and JS
* stream/concurrency controls
* examples

Guards:

* Lua/JS parity tests
* HTTP/1.1 workloads still unchanged

⸻

Phase 9 — gRPC

gRPC should follow HTTP/2, but not be merged into it.

P9-1: Unary gRPC

Deliverables:

* protobuf payload support strategy
* service/method naming
* metadata handling
* unary request/response
* status/trailer handling

Guards:

* E2E against test gRPC service
* invalid status tests
* latency attribution

P9-2: gRPC workload authoring

Deliverables:

* Lua helper API
* JS helper API
* descriptor/proto loading strategy, if in scope
* binary payload handling

Guards:

* Lua/JS parity tests
* example workload

P9-3: Streaming gRPC, optional/later

Deliverables:

* server streaming
* client streaming
* bidirectional streaming
* stream lifecycle metrics

Guards:

* long-lived stream tests
* cancellation tests
* per-message versus per-RPC metric decision

This may deserve a separate milestone because streaming gRPC is closer to a different workload model than a normal request/response protocol.

⸻

Phase 10 — HTTP/3 / QUIC

HTTP/3 should come after HTTP/2 and gRPC, not before.

Reason: HTTP/3 combines several hard things at once: QUIC transport, TLS 1.3 integration, stream multiplexing, connection migration semantics, and different failure modes. By the time you start HTTP/3, HTTP/2 should already have forced you to clarify the stream/request model.

P10-1: QUIC transport spike

Deliverables:

* select QUIC library
* basic connection
* TLS configuration
* ALPN
* connection lifecycle

Guards:

* connect/disconnect tests
* failure-mode tests

P10-2: HTTP/3 basic requests

Deliverables:

* HTTP/3 request/response
* QPACK integration
* GET/POST
* no advanced tuning initially

Guards:

* E2E against HTTP/3 server
* frame/header tests where practical

P10-3: HTTP/3 concurrency and metrics

Deliverables:

* concurrent streams
* per-stream latency
* connection-level metrics
* error/cancellation handling

Guards:

* multiplexed E2E
* latency attribution
* comparison with HTTP/2 behavior

⸻

Suggested final order

I would use this order:

Phase	Purpose	Why here
2	Redis vertical slice	Cheapest real proof of protocol-agnostic architecture
3	Extension boundary and packaging	Required before multiplying extensions
4	memcached	Cheap second extension; hardens extension model before DB complexity
5	JavaScript	Proves scripting abstraction is not Lua-shaped
6	PostgreSQL + MySQL	Stateful database protocols after extension/script foundations are proven
7	Request sources / ammo / replay	Proves Tank-like request-source layer
8	HTTP/2	First major multiplexing challenge
9	gRPC	Builds on HTTP/2
10	HTTP/3	Highest transport complexity; depends on stream model maturity

This is the key change from your current plan: do not put PostgreSQL, JavaScript, log replay, and HTTP/2/gRPC into one broad Phase 3. They validate different architectural seams and should become separate milestones.

Revised interpretation of the gates

Your current plan has one main gate:

Can the architecture support a non-HTTP protocol?

I would define several gates:

Gate A — protocol independence

Passed by Redis basic E2E.

Failure condition:

* Redis requires edits to engine/orchestrator core.

Gate B — outstanding-operation model

Passed by Redis pipelining.

Failure condition:

* latency/accounting/rate logic assumes exactly one in-flight request per connection.

Gate C — extension independence

Passed by Redis or memcached as a real loadable extension.

Failure condition:

* extensions need private core headers or special-case build wiring.

Gate D — scripting independence

Passed by the same Redis workload in Lua and JavaScript.

Failure condition:

* protocol helpers are coupled to Lua-specific types or lifecycle.

Gate E — stateful protocol complexity

Passed by PostgreSQL extended query protocol.

Failure condition:

* multi-step protocol state leaks into the scheduler/request layer.

Gate F — request-source independence

Passed by HAR or data-driven replay.

Failure condition:

* workload generation is script-shaped rather than request-source-shaped.

Gate G — multiplexing

Passed by HTTP/2 concurrent streams.

Failure condition:

* the engine cannot model multiple logical requests over one connection cleanly.

Gate H — transport abstraction

Passed by HTTP/3.

Failure condition:

* transport assumptions are TCP/TLS-specific and leak into protocol execution.

Specific changes I would make to your current phase text

Keep Redis in Phase 2, but split pipelining

Your instinct is right that Redis is the cheapest real proof. But pipelining deserves its own subphase because it tests a stronger claim than basic Redis.

Redis without pipelining proves:

non-HTTP request/response protocols can work.

Redis with pipelining proves:

the engine can account for multiple outstanding logical operations on one connection.

That is the same family of problem as HTTP/2 multiplexing, but cheaper.

Move JavaScript before PostgreSQL

JavaScript should come before large protocol expansion because otherwise each new extension may grow Lua-first APIs that later need redesign.

A better proof is:

1. Redis in Lua works.
2. Redis in JavaScript works.
3. Then PostgreSQL and MySQL are added with both scripting engines in mind.

Add memcached before PostgreSQL

memcached is useful as a low-cost “second extension” proof.

Redis might accidentally fit because you designed around it. memcached helps confirm the extension API is not Redis-shaped before you invest in database protocols.

Split PostgreSQL into simple query and extended query

Do not make the first PostgreSQL milestone include prepared statements.

PostgreSQL simple query proves:

stateful database connection/auth/result parsing works.

PostgreSQL extended query proves:

multi-step prepared-statement lifecycles work.

Those are different risks.

Split HTTP/2 and gRPC

HTTP/2 is an engine/concurrency stressor.

gRPC is a semantic/application-layer feature on top of HTTP/2.

Treating them as one task hides the real gate. First prove HTTP/2 streams and metrics. Then add gRPC framing, metadata, protobuf payloads, and status/trailers.

Add HTTP/3 only after HTTP/2/gRPC

HTTP/3 should not be in the same broad bucket as HTTP/2. It validates the transport abstraction, not just the protocol abstraction.

Condensed roadmap

The more granular roadmap I would use is:

Phase 2: Redis architecture gate
  P2-1 Redis protocol skeleton
  P2-2 Redis Lua bindings
  P2-3 Redis E2E
  P2-4 Redis pipelining
Phase 3: Extension system hardening
  P3-1 Extension API/ABI boundary
  P3-2 Extension manifest/loading/build support
  P3-3 Convert Redis into reference extension
Phase 4: memcached
  P4-1 memcached text protocol
  P4-2 memcached E2E
  P4-3 optional binary protocol
Phase 5: JavaScript
  P5-1 Script API conformance cleanup
  P5-2 QuickJS integration
  P5-3 JS Redis bindings
  P5-4 Lua/JS parity tests
Phase 6: Databases
  P6-1 PostgreSQL simple query
  P6-2 PostgreSQL extended query/prepared statements
  P6-3 PostgreSQL transactions/session behavior
  P6-4 MySQL simple query
  P6-5 MySQL prepared statements
Phase 7: Request sources / ammo
  P7-1 Request-source abstraction
  P7-2 HAR replay
  P7-3 CSV/JSON/data-driven workloads
  P7-4 protocol-neutral replay examples
Phase 8: HTTP/2
  P8-1 Stream/request model review
  P8-2 Basic HTTP/2
  P8-3 HTTP/2 multiplexing
  P8-4 HTTP/2 Lua/JS API
Phase 9: gRPC
  P9-1 Unary gRPC
  P9-2 gRPC Lua/JS helpers
  P9-3 optional streaming gRPC
Phase 10: HTTP/3
  P10-1 QUIC transport spike
  P10-2 Basic HTTP/3
  P10-3 HTTP/3 concurrency/metrics

Bottom-line recommendation

Your existing reasoning is strong, but I would change the structure from:

Phase 2: Redis gate
Phase 3: Everything else

to:

Phase 2: First non-HTTP proof
Phase 3: Extension boundary proof
Phase 4: Second cheap extension proof
Phase 5: Second scripting engine proof
Phase 6: Stateful database protocols
Phase 7: Request-source/ammo layer
Phase 8: Multiplexed HTTP/2
Phase 9: gRPC
Phase 10: HTTP/3

That gives you a cleaner progression:

simple protocol → real extension → second extension → second script engine → complex stateful protocols → alternate workload sources → multiplexed protocols → new transport.
