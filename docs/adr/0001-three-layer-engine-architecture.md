# 1. Three-Layer Engine Architecture

| Field         | Value          |
|---------------|----------------|
| Status        | Accepted       |
| Date          | 2026-06-04     |
| Phase         | Phase 1        |
| Deciders      | wrkx core team |
| Supersedes    | —              |
| Superseded by | —              |

---

## Context

wrkx is a load testing tool forked from wrk2. The current codebase is
approximately 8 300 lines of C99. The engine, protocol handling, scripting
layer, CLI, and output formatting are substantially co-located in `wrk.c`
(892 lines) with 55 static function prototypes in `main.h`.

The planned roadmap requires:

- Multiple network protocols: HTTP/1.1 (current), HTTP/2, gRPC, WebSocket,
  Redis, PostgreSQL, MySQL
- Multiple scripting engines: LuaJIT (current), JavaScript (QuickJS)
- User-defined sessions with per-connection state
- Log replay (HAR, pcap)
- Automated testing at unit, integration, and E2E levels
- AI-agent-driven development where tasks must have bounded, well-defined scope

The current architecture cannot accommodate this roadmap without deep surgery
on the core at each addition. HTTP parsing is hardwired into the event loop.
The scripting API is HTTP-shaped. Static linkage prevents unit testing of
individual functions. Every new protocol or scripting engine would require
changes across multiple concerns simultaneously.

A tactical refactor — splitting `wrk.c` into `cli.c`, `output.c`,
`engine.c` — was considered but rejected as insufficient. It reorganises code
without establishing the architectural boundaries that make independent
extension possible.

---

## Decision Drivers

1. **Protocol independence.** Adding Redis must not require changes to the
   rate controller or scripting layer.
2. **Scripting engine independence.** Adding JavaScript must not require
   changes to the protocol or transport layer.
3. **Testability.** Each layer must be unit-testable in isolation, without
   requiring a running network or scripting runtime.
4. **AI-dev safety.** Each implementation task must have a complete,
   machine-readable contract (a C header file) that defines exactly what
   must be produced. An agent implementing one layer must not need
   knowledge of another layer's internals.
5. **Validated pattern.** The architecture must map to a known, proven
   load-testing design.

---

## Considered Options

### Option A — Tactical file split

Split `wrk.c` into `cli.c`, `output.c`, `engine.c`, `main.c`. Remove
`main.h`. No layer boundaries defined. HTTP remains hardwired.

- Adding a protocol still requires changes in `engine.c`
- Scripting layer remains HTTP-shaped
- Does not address testability of core logic
- Faster to execute but solves none of the roadmap blockers

**Rejected.**

### Option B — Three-layer architecture (this decision)

Define three independent layers with explicit C interface contracts.
Implement each layer against its contract. Wire at startup via CLI.

- Each layer is independently testable and extensible
- Adding a protocol means implementing one header file
- Adding a scripting engine means implementing one header file
- Implementation tasks are bounded by interface contracts

**Accepted.**

---

## Decision

wrkx adopts a three-layer architecture modelled on Yandex.Tank's
separation of concerns:

```
┌─────────────────────────────────────────────────────┐
│  Request Layer  (Ammo)                               │
│  script_api.h — what to send, how to interpret       │
│  session.h    — per-connection script-visible state  │
├─────────────────────────────────────────────────────┤
│  Protocol Engine  (Machine Gun)                      │
│  proto.h      — protocol vtable (encode / decode)   │
│  transport.h  — TCP / TLS (selectable, independent) │
├─────────────────────────────────────────────────────┤
│  Orchestrator  (Tank)                                │
│  orchestrator.h — rate control, connection pool,    │
│                   stats, thread pool, lifecycle      │
└─────────────────────────────────────────────────────┘
```

### Layer responsibilities

**Orchestrator** knows nothing about protocols or request content.
It manages:
- Thread pool and ae event loop per thread
- Connection pool (N connections × T threads)
- Rate controller with Coordinated Omission correction (HdrHistogram)
- Stats aggregation (latency percentiles, RPS, error counts)
- Lifecycle: `init → connect → run → drain → report`

It asks the Protocol Engine one question: _"is the response complete?"_
It asks the Request Layer one question: _"what bytes should I send next?"_

**Protocol Engine** knows nothing about rate or scripting.
It manages:
- Transport selection: TCP or TLS, any protocol
- Protocol vtable: connect (including auth handshake), write, readable, close
- Per-connection protocol state (`proto_state`)
- Response completion detection (signals `DONE | PENDING | ERROR`)
- Protocol implementations: `http1.c`, `redis.c`, `pgsql.c`, etc.

**Request Layer** knows nothing about connections or timing.
It manages:
- Scripting engine implementations (LuaJIT, QuickJS)
- Hook contract: `init`, `request`, `response`, `done`
- Per-protocol helper registration (`redis.command()`, `pg.query()`, etc.)
- Session manager: script-visible per-connection key-value state
- Request generation and response interpretation

### Interface contracts

The three header files below are the decision made concrete.
Implementation must not cross layer boundaries.

**`orchestrator.h`**
```c
typedef struct {
    uint64_t connections;
    uint64_t threads;
    uint64_t duration_us;
    uint64_t rate;
} orchestrator_cfg;

typedef struct {
    struct hdr_histogram *latency;
    uint64_t requests;
    uint64_t errors_connect;
    uint64_t errors_status;
    uint64_t errors_timeout;
    uint64_t start_us;
    uint64_t elapsed_us;
} orchestrator_stats;

/* Opaque handle. */
typedef struct orchestrator orchestrator;

orchestrator *orchestrator_create(orchestrator_cfg, protocol *, script_engine *);
int           orchestrator_run(orchestrator *);
orchestrator_stats orchestrator_collect(orchestrator *);
void          orchestrator_destroy(orchestrator *);
```

**`proto/proto.h`**
```c
typedef enum { PROTO_PENDING, PROTO_DONE, PROTO_ERROR } proto_status;

typedef struct connection connection;

typedef struct protocol {
    const char   *name;

    /* Called once per connection. Includes auth handshake if required.
       Allocates and assigns conn->proto_state. */
    int  (*connect)(connection *);

    /* Encode and send a request. buf/len are owned by the Request Layer. */
    int  (*write)(connection *, const char *buf, size_t len);

    /* Called by ae on each readable event. Buffers internally.
       Returns DONE when a complete response has been received,
       PENDING if more bytes are expected, ERROR on failure. */
    proto_status (*readable)(connection *);

    /* Frees conn->proto_state. Closes the socket. */
    void (*close)(connection *);
} protocol;

/* Shared connection structure. proto_state is owned by the protocol. */
struct connection {
    int      fd;
    void    *proto_state;   /* opaque; allocated by connect, freed by close */
    void    *script_state;  /* opaque; owned by the Request Layer */
    /* transport, thread back-pointer, timing fields — internal */
};
```

**`scripting/script_api.h`**
```c
typedef struct script_engine script_engine;
typedef struct session        session;

typedef struct script_api {
    const char *name;

    script_engine *(*create)(const char *file);

    /* Called once per thread before any requests. */
    void (*init)(script_engine *, uint64_t thread_id, uint64_t connections);

    /* Called before each request. Returns heap-allocated buffer; engine frees. */
    char *(*request)(script_engine *, size_t *len_out);

    /* Called after each completed response. status is protocol-defined. */
    void (*response)(script_engine *, int status, size_t bytes, uint64_t latency_us);

    /* Called once after the run completes. */
    void (*done)(script_engine *, orchestrator_stats *);

    void (*destroy)(script_engine *);
} script_api;

/* Engine-agnostic helper descriptor.
   engine_ctx is supplied by the scripting engine at each call site.
   Its concrete type (lua_State *, JSContext *, ...) is known only to
   the glue module that implements fn — never to the protocol layer.
   Argument marshalling and return value handling are the engine's
   responsibility. */
typedef int (*script_helper_fn)(void *engine_ctx);

typedef struct script_helper {
    const char        *name;
    script_helper_fn   fn;   /* implemented in scripting/<engine>/<proto>_helpers.c */
} script_helper;

/* Registers a namespace of helpers into the scripting engine.
   Called by each scripting/<engine>/<proto>_helpers.c during engine init.
   NOT called by proto/*.c — protocol implementations have no scripting
   knowledge. */
void script_register_helpers(script_engine       *,
                              const char          *ns,
                              const script_helper *helpers,
                              size_t               count);

/* Session: per-connection key-value store accessible from scripts. */
session    *session_create(void);
void        session_set(session *, const char *key, const char *value);
const char *session_get(session *, const char *key);
void        session_destroy(session *);
```

### Glue module pattern

Protocol helper functions that expose protocol behaviour to scripts are
implemented in per-engine glue modules, not in the protocol itself:

```
proto/redis.c                       pure C encode/decode, no scripting
scripting/lua/redis_helpers.c       Lua call convention, calls proto/redis.c
scripting/js/redis_helpers.c        JS call convention, calls proto/redis.c
```

A glue module knows exactly two things: the scripting engine's native call
convention, and the protocol's public C API. Neither the protocol nor the
scripting engine interface knows about the other.

```c
/* scripting/lua/redis_helpers.c — example only, not part of this ADR */
#include "scripting/script_api.h"
#include "proto/redis.h"            /* proto C API, no lua.h needed in redis.h */
#include <lua.h>                    /* Lua call convention */

static int lua_redis_command(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    const char *cmd = luaL_checkstring(L, 1);
    /* ... call redis_encode_command(cmd) from proto/redis.h ... */
    return 1;
}

static const script_helper redis_helpers[] = {
    { "command", lua_redis_command },
    { NULL, NULL }
};

void register_redis_helpers_lua(script_engine *engine) {
    script_register_helpers(engine, "redis", redis_helpers,
                             sizeof(redis_helpers)/sizeof(redis_helpers[0]) - 1);
}
```

### Invariants

1. `orchestrator.c` must not `#include` any protocol or scripting header
   other than `proto.h` and `script_api.h`.
2. `proto/*.c` (all protocol implementations) must not `#include` any
   scripting header. Protocol files must not reference `lua.h`, `quickjs.h`,
   or any other engine header.
3. `scripting/<engine>/engine.c` must not `#include` any protocol header
   directly. Protocol behaviour is accessed only through glue modules.
4. Glue modules (`scripting/<engine>/<proto>_helpers.c`) may include both
   a protocol header and an engine header. This is the only location where
   both are permitted to coexist.
5. All cross-layer communication passes through the interfaces above.
   Direct function calls between layers outside of glue modules are a
   violation.

---

## Implementation Sequence

P1-1 is the only task that must complete before P1-2, P1-3, and P1-4.
After P1-1 those three tasks are independent and can proceed in parallel.

```
P1-1  Define contracts (headers only, zero implementation)
       └─ P1-2  Implement Orchestrator
       └─ P1-3  Implement Protocol Engine (transport + HTTP/1.1)
       └─ P1-4  Implement Request Layer (Lua + helpers + session)
              └─ P1-5  Wire (CLI, main.c), full E2E suite green
```

P1-1 completion criterion: all three headers compile cleanly against a
stub `main.c` that declares but does not call anything. No implementation
files exist yet.

---

## Phase 1 Migration Map

`src/wrk.c` (~1 000 lines) is the single file being decomposed. The table below
maps every top-level symbol currently in `wrk.c` to its target layer/module, so
that P1-2/P1-3/P1-4 implementers know where each piece lands. It is intentionally
group-level (not line-numbered) so it does not go stale as code moves.

| Current `wrk.c` symbols | Target layer / module |
|---|---|
| `print_stats_header`, `print_units`, `print_stats`, `print_hdr_latency` | Orchestrator — report stage (CLI-side reporter) |
| `usage`, `longopts[]`, `parse_args`, `main`, `copy_url_part` | CLI / wiring (P1-5) — builds `orchestrator_cfg` |
| `connect_socket`, `reconnect_socket`, `delayed_initial_connect`, `socket_connected` | Protocol Engine — `transport.c` + `proto->connect` |
| `header_field`, `header_value`, `response_body`, `response_complete`, `parser_settings`, `sock` | Protocol Engine — `proto/http1.c` (= `proto->readable`) |
| `socket_writeable`, `socket_readable` | Orchestrator ae glue → delegate to `proto->write` / `proto->readable` |
| `usec_to_next_send`, `delay_request`, `calibrate`, `check_timeouts`, `sample_rate` | Orchestrator — rate controller (`rate.c` sub-module) + CO correction |
| `thread_main`, `progress_main`, `handler` (SIGINT) | Orchestrator — thread pool + lifecycle (signal-driven drain) |
| `time_us` | shared util (`utils.h`) |
| `cfg`, `statistics`, `stop`, `g_calibrated_threads`, `g_progress_done` | Orchestrator internal state — folded into the opaque `orchestrator` handle (no `globals.h`) |

**A layer is a boundary, not a single file.** The Orchestrator layer may contain
several sub-modules (`orchestrator.c`, `rate.c`, the existing `stats.c`); the
Protocol Engine may contain `transport.c` plus one `proto/*.c` per protocol. The
constraint is the interface contract and the no-cross-layer-include invariants
above — not the file count. This is what keeps any single module from growing back
into a god-file.

---

## Consequences

### Positive

- Adding any new protocol requires implementing `proto.h` only. Zero
  changes to the Orchestrator or Request Layer.
- Adding any new scripting engine requires implementing `script_api.h`
  only. Zero changes to the Orchestrator or Protocol Engine.
- Each layer is independently unit-testable: the Orchestrator can be
  tested with a stub protocol; a protocol can be tested by feeding it
  raw bytes without a running engine.
- P1-2, P1-3, P1-4 are parallel tasks with complete, self-contained
  contracts. An AI agent implementing P1-3 requires no knowledge of
  P1-4's internals.
- The architecture maps directly to a validated pattern (Yandex.Tank).

### Negative

- P1-1 (interface design) must be stable before implementation begins.
  Changes to the contracts after P1-2/P1-3/P1-4 have started are
  expensive.
- The `session.h` / `proto_state` design adds indirection absent from
  the current codebase. Debugging across layers requires understanding
  the ownership rules.
- Initial implementation effort is higher than a tactical file split.

### Neutral

- The existing LuaJIT scripting and HTTP/1.1 behaviour must be preserved
  identically. Compliance is verified by the Phase 0 golden output test
  and the E2E latency and reconnect tests.
- `static` functions in `wrk.c` that currently cannot be unit-tested
  will gain external linkage as a side-effect of extraction, enabling
  the unit test tasks from Phase 0.

---

## Compliance

A pull request violates this ADR if any of the following are true:

- `orchestrator.c` includes a protocol or engine-specific header
- A `proto/*.c` file includes `lua.h`, `quickjs.h`, or any other
  engine header
- A `scripting/<engine>/engine.c` includes a protocol header directly
  (without going through a glue module)
- A new protocol is added by modifying `orchestrator.c` or any
  scripting file rather than by adding a new `proto/*.c`
- A new scripting engine is added by modifying `orchestrator.c` or
  any protocol file rather than by adding a new `scripting/*/engine.c`
- A glue module is absent when a protocol provides helpers
  (`scripting/<engine>/<proto>_helpers.c` must exist for each
  protocol × engine combination that exposes helpers)

Verification:

```sh
# No engine headers in protocol layer
grep -r 'lua\.h\|quickjs\.h' src/proto/ && echo VIOLATION

# No protocol headers in engine implementations (glue modules excluded)
grep -r '#include.*proto/' src/scripting/*/engine.c && echo VIOLATION

# No engine headers in orchestrator
grep -r 'lua\.h\|quickjs\.h' src/orchestrator.c && echo VIOLATION
```

