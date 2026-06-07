title: script API contract cleanup — vtable helper registration + capability detection
status: completed
adr: 0005
adr-step: P5-1
depends: t068

## Outcome

Done. `script_api` gained `capabilities` and `register_helpers` vtable slots; the
global `script_register_helpers` became engine-internal `lua_register_helpers`
(exported via `lua/engine.h`, wired into the vtable). LuaJIT `capabilities()`
reports `SCRIPT_CAP_DYNAMIC_REQUEST`/`SCRIPT_CAP_RESPONSE_HOOK` by detecting
user-defined global `request`/`response` functions (wrk.lua's static default
lives on `wrk.request`, never as a global). Engine-tagged-namespace selection
lives in the new header-only `src/scripting/helper_tag.h` (`helper_ns_select`),
applied in `main.c`; redis/memcached extensions now register `redis@lua` /
`memcached@lua`.

Implementation notes vs the plan:
- Capability detection uses presence of global `request`/`response` rather than a
  "diff against the wrk.lua default reference" — wrk.lua installs no global
  `request` (it sets `wrk.request` inside `wrk.init`), so global presence is the
  exact, simpler signal for "user overrode".
- The selection predicate was extracted to `helper_tag.h` (header-only, inline)
  so both `main.c` and the unit test exercise identical logic.
- `extensions/redis/init.c` added to the `test_redis_lua` link deps so the redis
  `@lua` tagging audit runs as a unit test (memcached's audit reused the existing
  `test_memcached_extension` registration test).

Verified: `make test` and `make test-asan` green (incl. memcached/redis e2e and
release-binary smoke); `include/wrkx_extension.h` and `src/orchestrator.c` git
diffs empty.

## Why this is the first Phase 5 task

Two pieces of the current Request Layer are Lua-shaped and block a second engine.
Both must be fixed *before* any QuickJS code is written, against LuaJIT only, with
behaviour preserved.

### Problem 1 — helper registration is a global symbol bound to LuaJIT

`script_register_helpers(...)` is declared in `src/scripting/script_api.h:66` but
is a **single global function implemented in `src/scripting/lua/engine.c:104`**.
`main.c:295` calls it directly:

```c
for (int i = 0; i < g_helper_set_count; i++)
    script_register_helpers(eng, g_helper_sets[i].ns, ...);
```

If QuickJS is linked into the same binary it cannot provide its own
implementation — two definitions of the same global symbol is a link error.
Helper registration must move into the `script_api` vtable so each engine carries
its own.  (Note: `script_register_helpers` lives in `script_api.h`, *not*
`include/wrkx_extension.h` — the extension-facing API is untouched, so Gate C/C'
stay intact.)

### Problem 2 — no language-neutral capability detection

`t->dynamic` is hardcoded `false` (`orchestrator.c:687`) and the request body is
generated **once** before workers spawn (`orchestrator.c:718`).  Every Lua script
defines a `request()` (from the `wrk.lua` default), so "the engine has a request
hook" cannot mean "generate per request" — that would silently convert every
static HTTP workload into a per-request scripting workload and start delivering
`response()` callbacks to scripts that never asked for them.

The orchestrator needs an engine-reported, language-neutral capability bitmask so
it can decide *per workload* whether to run static or dynamic, without knowing
anything about Lua or JS.

### Problem 3 — the helper ABI itself is Lua-shaped (decision required)

Moving `register_helpers` into the vtable fixes *dispatch*, but the helper
**function ABI** is still Lua-specific.  `script_helper_fn` is:

```c
typedef int (*script_helper_fn)(void *engine_ctx);   /* wrkx_extension.h:68 */
```

and every existing helper casts `engine_ctx` to `lua_State *` and drives the Lua
stack directly (`extensions/redis/redis_lua_helpers.c:29`):

```c
static int lua_redis_command(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    int argc = lua_gettop(L);
    ... lua_tolstring(L, ...); ... lua_pushlstring(L, buf, len); ...
}
```

A QuickJS engine cannot execute these bodies — there is no Lua stack to read.

**Decision (this task records it):** keep the `wrkx_extension.h` ABI **frozen** —
`script_helper_fn` stays `int(void *engine_ctx)`, preserving Gate C/C'.  Do *not*
invent a marshalled language-neutral arg/return vector (that would change the
extension contract and rewrite every existing helper).  Instead:

1. **Helper tables are engine-specific.**  Each extension provides one glue table
   per engine — the existing `redis_lua_helpers[]` plus a future
   `redis_quickjs_helpers[]` (t075).  The engine-neutral seam they share is the
   protocol encoder (`redis_make_request()`), which has no scripting dependency.
2. **`engine_ctx` is an engine-defined context.**  Lua passes `lua_State *` (as
   today).  QuickJS passes a small bundle (`{ JSContext *, int argc, JSValueConst
   *argv, JSValue *ret }`); its helpers cast `engine_ctx` to that bundle, read
   `argv`, call the shared encoder, and write `*ret`.  The `int(void*)` signature
   is unchanged.
3. **The right table reaches the active engine via an engine-tagged namespace
   convention — no `wrkx_extension.h` change.**  This is the key plumbing detail
   the earlier draft left hand-wavy.  Facts on the ground:
   - The extension entry point receives `const wrkx_extension_api *`
     (`extensions/redis/init.c:35`), which has **no** engine name.
   - `wrkx_extension_api.register_helpers(ns, helpers, count)` is the only hook,
     and the host registry (`helper_set` in `main.c:69`) stores only
     `{ns, helpers, count}` — no engine discriminator.

   Rather than add a field to the frozen extension API, encode the target engine
   in the **namespace string**:
   - `register_helpers("foo", ...)` — engine-agnostic, applied to every engine.
     Use this only when the helper bodies do not assume a VM-specific
     `engine_ctx`.
   - `register_helpers("redis@lua", ...)` / `register_helpers("redis@quickjs", ...)`
     — applied only when the active engine's name matches the `@<engine>` suffix;
     bound under the bare namespace (`"redis"`).

   Because the existing Redis and memcached helper bodies cast `engine_ctx` to
   `lua_State *`, they are **not** engine-agnostic.  When the host filter lands,
   all existing Lua-shaped extension helper tables must be registered with
   `@lua` tags.  Truly engine-neutral helpers (for example a no-op helper that
   ignores `engine_ctx`) may remain untagged.

   The selection happens **host-side** in `main.c`, which knows the active engine
   (`api->name` of the chosen `script_api`).  `include/wrkx_extension.h` is not
   touched, so Gate C/C' hold exactly.

This task does **not** implement the QuickJS table (that is t075) — it fixes the
decision, the Lua side, and the host selection logic so the contract is
unambiguous before QuickJS exists.

## Deliverables

### 1. `src/scripting/script_api.h` — two vtable additions

```c
/* Capability bits an engine reports for the currently loaded script. */
typedef enum {
    SCRIPT_CAP_DYNAMIC_REQUEST = 1u << 0, /* call request() for every request   */
    SCRIPT_CAP_RESPONSE_HOOK   = 1u << 1, /* deliver response() callbacks        */
} script_cap;

typedef struct script_api {
    const char *name;
    script_engine *(*create)(const char *file);
    int  (*configure)(script_engine *, const char *url,
                      const char * const *headers, size_t n_headers);

    /* Report capability bits for the loaded script. NULL ⇒ treat as 0 (fully
     * static: pre-generate one request, no response callbacks). Called once
     * per engine after configure() and helper registration, before init(). */
    uint32_t (*capabilities)(script_engine *);

    /* Register a namespace of protocol helpers into THIS engine. Replaces the
     * former global script_register_helpers(). NULL ⇒ engine has no helper
     * support. */
    void (*register_helpers)(script_engine *, const char *ns,
                             const script_helper *helpers, size_t count);

    void  (*init)(script_engine *, uint64_t thread_id, uint64_t connections);
    char *(*request)(script_engine *, size_t *len_out);
    void  (*response)(script_engine *, int status, size_t bytes, uint64_t latency_us);
    void  (*done)(script_engine *, struct orchestrator_stats *);
    void  (*destroy)(script_engine *);
} script_api;
```

Remove the standalone `void script_register_helpers(...)` declaration from the
public contract.  Keep an engine-internal equivalent for Lua glue (below).

### 2. `src/scripting/lua/engine.c` — implement both slots

**`register_helpers`:** rename the existing global `script_register_helpers` to a
file-local `lua_register_helpers(script_engine *, ...)`, keep its body verbatim,
and assign it to `lua_api.register_helpers`.  Lua glue modules
(`lua/http1_helpers.c`, memcached glue) call `lua_register_helpers` directly
(same translation unit family / engine.h export) rather than the old global.

**`capabilities`:** detect what the *user* script overrode, distinct from the
`wrk.lua` defaults:
- After loading `wrk.lua`, capture the default `request` function reference.
- After loading the user script, compare: if `request` differs from the default,
  set `SCRIPT_CAP_DYNAMIC_REQUEST`.
- If the user script defines a `response` function, set `SCRIPT_CAP_RESPONSE_HOOK`.
- A NULL script (`create(NULL)`) reports `0` — pure static default GET.

Document this comparison in a comment; it is the Lua-specific realization of the
language-neutral contract.

### 3. `src/scripting/lua/engine.h` — export `lua_register_helpers`

So `lua/http1_helpers.c` and the memcached glue can call it without the old
global.

### 4. `src/main.c` — use the vtable

Use the vtable slot *and* apply the engine-tagged-namespace selection (Problem 3,
point 3).  A namespace `"ns@engine"` is bound only when `engine` matches
`api->name`; an untagged `"ns"` is bound for every engine.  The `@engine` suffix
is stripped before binding so the script always sees the bare namespace:

```c
for (int i = 0; i < g_helper_set_count; i++) {
    const char *ns  = g_helper_sets[i].ns;
    const char *at  = strchr(ns, '@');
    if (at) {                                   /* engine-tagged set */
        if (strcmp(at + 1, api->name) != 0) continue;   /* not this engine */
        size_t base = (size_t)(at - ns);
        char bare[64];
        memcpy(bare, ns, base); bare[base] = '\0';      /* "redis@lua" -> "redis" */
        ns = bare;
    }
    if (api->register_helpers)
        api->register_helpers(eng, ns,
                              g_helper_sets[i].helpers, g_helper_sets[i].count);
}
```

This is the only added main.c logic.  `g_helper_sets` / `helper_set` keep their
existing `{ns, helpers, count}` shape — the engine tag rides inside the `ns`
string, so `include/wrkx_extension.h` is untouched.  The orchestrator is **not**
touched in this task — capability *consumption* is wired in t070.

### 5. Existing Lua-shaped extension registrations — add `@lua` tags

Retag every current helper table whose functions assume `engine_ctx == lua_State *`:

```c
/* extensions/redis/init.c */
api->register_helpers("redis@lua", redis_lua_helpers, redis_lua_helpers_count);

/* extensions/memcached/init.c */
api->register_helpers("memcached@lua", mc_lua_helpers, mc_lua_helpers_count);
```

The host strips the suffix before binding, so Lua scripts still see `redis.*`
and `memcached.*`.  This preserves existing Lua behavior while preventing these
Lua-shaped tables from being bound into QuickJS later.

### 6. `src/scripting/lua/http1_helpers.c`, memcached glue — call rename

Replace `script_register_helpers(engine, ...)` with `lua_register_helpers(engine, ...)`.

### 7. `tests/unit/test_lua_engine.c` — capability + registration tests

```
test_capabilities_null_script_is_static
  - create(NULL) → capabilities() == 0

test_capabilities_static_request_is_static
  - script defines only default-style behaviour (no custom request) → 0
    (or whichever default the wrk.lua baseline yields)

test_capabilities_custom_request_is_dynamic
  - script: function request() return wrk.format("GET","/"..math.random()) end
  - capabilities() & SCRIPT_CAP_DYNAMIC_REQUEST != 0

test_capabilities_response_hook_detected
  - script defines function response(status,...) end
  - capabilities() & SCRIPT_CAP_RESPONSE_HOOK != 0

test_register_helpers_via_vtable
  - api->register_helpers(eng,"foo",helpers,1); helper callable from Lua

test_engine_tagged_namespace_selection (host selection logic)
  - helper sets {"a", "b@lua", "c@quickjs"}; active engine "lua"
  - "a" and "b" bound (b stripped of @lua); "c" skipped

test_lua_shaped_extension_helpers_are_tagged
  - Redis and memcached init paths register "redis@lua" / "memcached@lua"
  - no Lua-shaped extension helper table is left untagged
```

## Guards

- `make test` and `make test-asan` pass unchanged (existing Lua/HTTP/memcached
  tests green — behaviour preserved)
- `./wrkx -t4 -c100 -d3s -R100 http://localhost` produces the same throughput as
  before (still static; capabilities consumed only in t070)
- `include/wrkx_extension.h` git diff is empty (extension API untouched)
- `src/orchestrator.c` git diff is empty (orchestrator untouched in this task)

## Core engine touch

`script_api.h` (Request Layer contract) gains two vtable slots and loses one
global — this is the deliberate P5-1 contract cleanup.  `main.c` switches one
call site to the vtable.  The orchestrator, scheduler/event-loop, and protocol
layers are **not** touched.
