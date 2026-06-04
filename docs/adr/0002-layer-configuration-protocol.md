# 2. Layer Configuration Protocol

| Field         | Value                        |
|---------------|------------------------------|
| Status        | Accepted                     |
| Date          | 2026-06-05                   |
| Phase         | Phase 1                      |
| Deciders      | wrkx core team               |
| Supersedes    | —                            |
| Superseded by | —                            |
| Amends        | [0001](0001-three-layer-engine-architecture.md) §"Interface contracts" |

---

## Context

During the parallel implementation of the three layers (P1-2, P1-3, P1-4 of ADR
0001) three distinct contract gaps were independently discovered by each
implementing agent. All three share the same root cause: **the P1-1 contracts
define no configuration pathway from the wiring layer (CLI / `main.c`) into the
protocol or scripting layer before the run begins**. The ADR 0001 lifecycle is
`init → connect → run → drain → report` but has no `configure` step between "objects
created" and "orchestrator started".

### Gap 1 — `orchestrator_create` receives `script_engine *` but not `script_api *`

```c
/* ADR 0001 — ambiguous */
orchestrator *orchestrator_create(orchestrator_cfg,
                                  struct protocol *,
                                  struct script_engine *);
```

The orchestrator must call `api->request`, `api->response`, and `api->done` on
every request cycle, but the `script_api *` vtable is not passed at creation time.
The engine and its vtable are separate objects (`script_api` is the vtable descriptor;
`script_engine` is the per-run state created by `script_api.create()`). Without the
vtable the orchestrator cannot invoke any hook. The P1-2 implementation stored
`api = NULL` and went NULL-safe throughout — scripting hooks are unreachable.

### Gap 2 — `protocol.connect(connection *)` has nowhere to receive the connect target

```c
/* ADR 0001 — insufficient */
int (*connect)(connection *);
```

`struct connection` carries only `fd`, `proto_state`, and `script_state`. There is no
channel for the resolved `addrinfo *`, `SSL_CTX *`, or SNI hostname needed by the
protocol's connect implementation. The P1-3 implementation worked around this via a
process-scoped module-static `http1_configure()` that P1-5 (wiring) must call before
`orchestrator_run()`. The workaround is safe for one-target-per-process (current CLI
semantics) but is undocumented in any contract.

### Gap 3 — `script_api.create(file)` has nowhere to receive the URL and custom headers

```c
/* ADR 0001 — insufficient */
script_engine *(*create)(const char *file);
```

The legacy `script_create(file, url, headers)` received the target URL and any
custom headers, from which it set `wrk.scheme`, `wrk.host`, `wrk.port`,
`wrk.path`, and `wrk.headers`. The frozen `create(file)` can supply only defaults
(`scheme=http`, `host=localhost`, `path=/`). P1-5 wiring cannot configure the Lua
engine for the actual target without a new configuration channel.

### Out of scope — response headers/body and done histogram

The `response(engine, status, bytes, latency_us)` hook omits the response headers
and body that the legacy `response(status, headers, body)` Lua hook received.
Likewise, `done(engine, orchestrator_stats *)` exposes only scalar counters rather
than the `wrk.stats` histogram userdata that the legacy `done(summary, latency,
requests)` hook received.

Surfacing headers/body requires the protocol vtable to extract them after
`PROTO_DONE` and pass them up through a new channel. Richer `done` stats require
exposing a histogram API through the `orchestrator_stats` struct or a separate
accessor. These are meaningful changes to both `proto.h` and `orchestrator.h` and
are **deferred to a future ADR**. They are noted here so the omission is explicit
and traceable. Phase 1 behavioural equivalence for the tested scenarios is
maintained: the `response` hook is called with correct `status`; existing E2E
scripts that do not inspect headers/body pass without modification.

---

## Decision Drivers

1. **Unblock P1-5 (wiring).** Without these fixes, `orchestrator_run()` cannot
   call any scripting hook and the Lua engine cannot be configured with the actual
   target URL.
2. **Minimal contract surgery.** Each fix should be as small as possible; the P1-2,
   P1-3, and P1-4 implementations must remain valid with only additive changes.
3. **Invariant preservation.** None of the fixes may introduce cross-layer includes
   or violate the invariants defined in ADR 0001.
4. **One-target-per-process scope.** The current CLI invocation model (one host, one
   protocol, one scripting engine per `wrkx` run) is the only scope addressed here.
   Multi-target or dynamic-target configurations are a future concern.

---

## Considered Options

### Gap 1 — pass `script_api *` to `orchestrator_create`

**Option A (chosen):** add `const script_api *` as the third parameter, between
`protocol *` and `script_engine *`. The wiring layer already has both (it calls
`lua_script_api()` to get the vtable, then `api->create(file)` to get the engine).

**Option B:** engine carries its vtable — add a `const script_api *api` public
field or `script_api *script_engine_api(script_engine *)` accessor. Avoids changing
`orchestrator_create` but adds a convention not expressed in the current header.

Option A is more explicit and puts the contract change where the dependency is visible
(the orchestrator interface), not hidden inside the opaque engine.

### Gap 2 — protocol connect target

**Option A (chosen):** bless the `<proto>_configure()` module-level call pattern
already established by the P1-3 implementation. The `protocol` vtable gains **no new
slot**. The wiring layer (P1-5) calls `http1_configure(addr, ssl_ctx, host)` before
`orchestrator_run()`. Document this as the accepted convention for all future
protocol implementations: each must expose a `<proto>_configure()` function whose
signature is protocol-specific, callable by the wiring layer before the run.

**Option B:** add `int (*configure)(void *cfg)` to the `protocol` vtable with a
per-protocol struct cast inside. Uniform but fully opaque; loses type safety at the
vtable boundary and adds a nullable slot the orchestrator must call — but the
orchestrator then becomes aware of a "configure phase" that is really a wiring
concern.

Option A keeps the vtable clean and the orchestrator unaware of the distinction
between "connect to _this_ target" and "here is a pre-resolved target". The wiring
layer already knows which protocol it selected; it is the right place to call
`http1_configure()`.

### Gap 3 — script engine URL/headers configuration

**Option A (chosen):** add a `configure` slot to `script_api`, called once per
engine after `create()` and before `init()`. Signature:

```c
int (*configure)(script_engine *, const char *url,
                 const char * const *headers, size_t n_headers);
```

`url` is the full target URL string (e.g. `http://host:8080/path`). `headers` is
the array of raw header strings supplied on the CLI (`-H`). The engine parses `url`
to set `wrk.scheme/host/port/path` and installs each header into `wrk.headers`, as
the legacy `script_create` did. Future non-HTTP engines receive the same call and
ignore what they do not understand.

**Option B:** extend `create(file)` to `create(file, url, headers, n)`. Changes the
one slot already in use; harder to provide a backward-compatible NULL-safe fallback.

Option A is additive (new slot). The slot may be `NULL` for engines that need no
configuration; the wiring layer checks before calling.

---

## Decision

### 1. `src/orchestrator.h` — add `const script_api *` to `orchestrator_create`

```c
/* Forward declaration added alongside the existing struct forw-decls: */
struct script_api;

/* Revised signature: */
orchestrator *orchestrator_create(orchestrator_cfg,
                                  struct protocol *,
                                  const struct script_api *,
                                  struct script_engine *);
```

The `const struct script_api *` is the vtable; `struct script_engine *` is the
per-run engine instance created by `api->create()`. Passing both makes the
distinction explicit and lets the orchestrator call all hooks without an additional
accessor.

### 2. `src/proto/proto.h` — no change

The `protocol` vtable is unchanged. The accepted convention for protocol
configuration is:

> Every protocol implementation **must** expose a module-level function with the
> signature `int <proto>_configure(...)` where `...` is the protocol's own parameter
> set. The wiring layer (CLI / `main.c`) calls this function once, after address
> resolution, before calling `orchestrator_run()`. The orchestrator itself does
> not call `configure` and has no awareness of it.

For HTTP/1.1: `int http1_configure(struct addrinfo *, SSL_CTX *, const char *host)`.
Future protocols follow the same pattern with their own parameter sets.

### 3. `src/scripting/script_api.h` — add `configure` slot after `create`

```c
typedef struct script_api {
    const char *name;

    script_engine *(*create)(const char *file);

    /* NEW — called once per engine after create(), before any init().
     * url is the full target URL (scheme://host:port/path).
     * headers is an array of n_headers raw header strings (e.g. "X-Foo: bar").
     * May be NULL; wiring checks before calling. */
    int (*configure)(script_engine *, const char *url,
                     const char * const *headers, size_t n_headers);

    /* Unchanged below this line. */
    void (*init)(script_engine *, uint64_t thread_id, uint64_t connections);
    char *(*request)(script_engine *, size_t *len_out);
    void (*response)(script_engine *, int status, size_t bytes,
                     uint64_t latency_us);
    void (*done)(script_engine *, struct orchestrator_stats *);
    void (*destroy)(script_engine *);
} script_api;
```

---

## Impact on existing P1-2 / P1-3 / P1-4 implementations

**`src/orchestrator.c` (P1-2):**
- `orchestrator_create` must accept and store the new `const script_api *` parameter.
  Replace the NULL-initialised `o->api` with the passed value.
- The NULL-safety guards added in P1-2 remain valid (they protect the unit-test
  stub path where `api` is still NULL).

**`src/proto/http1.c` (P1-3):**
- No changes required; `http1_configure()` already exists and is the accepted pattern.

**`src/scripting/lua/engine.c` (P1-4):**
- Add `lua_configure(script_engine *, url, headers, n)` that parses `url` (using
  `http_parser_parse_url`) and sets `wrk.scheme/host/port/path`; iterates `headers`
  and installs each into `wrk.headers`. Match the legacy `script_create` behaviour.
- Set `lua_script_api()->configure = lua_configure` in the vtable.

**`src/main.c` (P1-5 stub):**
- The stub declares but does not call; it must be updated to declare the new
  `orchestrator_create` signature correctly. Since it only declares (no call),
  the update is one line.

---

## Implementation Sequence

This amendment is prerequisite work for **t029 (P1-5 wiring)**:

```
amend orchestrator.h   (one-line signature change)
amend script_api.h     (one new slot)
update orchestrator.c  (accept and store script_api *)
update lua/engine.c    (implement configure slot)
update main.c stub     (forward-declaration consistency)
──────────────────────────────────────────────────────
t029  Wire CLI + main — calls http1_configure(), api->configure(), orchestrator_create()
```

All five amendments are small and can be done in a single task/commit before or as
part of t029.

---

## Consequences

### Positive

- P1-5 (wiring) can call all scripting hooks. The orchestrator drives the full
  `request → response → done` cycle through the `script_api` vtable.
- The Lua engine can set `wrk.scheme/host/port/path` correctly for any target URL,
  restoring the existing scripting behaviour that the legacy `script_create` provided.
- `http1_configure()` pattern is documented and generalisable — future protocols know
  the expected pre-run contract.
- No invariant is violated. The three headers remain independent; no cross-layer
  include is introduced.

### Negative

- `orchestrator_create` grows a fourth parameter. Callers (currently only the test
  stub and soon `main.c`) must be updated. The unit-test stub passes `NULL` for the
  api (already handled by the NULL-safety guards).
- `script_api.configure` introduces a nullable slot that every implementation must
  handle. The pattern (check for NULL before calling; default to NULL in stubs)
  mirrors the existing `response`/`done` NULL-safety convention.

### Neutral

- Gaps 3b (response headers/body; done histogram) remain unaddressed. This is
  intentional for Phase 1. They are recorded here; a future ADR addresses them when
  the protocol vtable is extended with a response-data accessor.

---

## Compliance

After this amendment, a pull request violates the combined ADR 0001 + 0002 contract
if any of the following are true:

- A protocol implementation does not expose a `<proto>_configure()` function
- A scripting engine's `configure` slot is absent but the engine reads `wrk.host`
  or `wrk.path` from somewhere other than a `configure` call
- `orchestrator_create` is called without passing the `script_api *` (except in
  unit-test stubs where `NULL` is explicitly intended)
- Any of the ADR 0001 invariants (1–5) are violated

The ADR 0001 compliance grep checks are unchanged.
