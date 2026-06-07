title: per-thread engine clone + dynamic-mode orchestrator wiring
status: pending
adr: 0005
adr-step: P5-1
depends: t069

## Why

Dynamic per-request scripting (needed for Redis counter workloads and the Gate D
parity test) requires each thread to own an isolated engine — LuaJIT and QuickJS
are both single-threaded VMs.  Today all threads share one `lua_State *`
(`orchestrator.c:686`) and the t068 fix avoids the race only because
`t->dynamic` is hardcoded `false`.  This task adds a `clone` hook with fully
specified replay semantics, then turns dynamic mode on safely using the
`capabilities()` bitmask from t069 — never "request hook exists."

## The clone replay problem

A template engine is the product of a sequence, not just a file.  The order below
mirrors how `main.c` actually builds the template today (`create` → helper
registration at `main.c:294` → `configure` at `main.c:300` → `init` at
`main.c:306`); clone must replay the **same** order so a clone is identical to the
template:

1. `create(file)` — `file` may be **NULL** (default `wrk.lua` workload)
2. `register_helpers(ns, ...)` × N — one per active extension namespace
3. `configure(url, headers, n)` — injects scheme/host/port/path/headers into the VM
4. `init(thread_id, connections)` — per-thread state

**`init()` is currently called in the wrong place.**  `main.c:306` runs
`api->init(eng, 0, args.cfg.connections)` on the template *before* the
orchestrator exists, so it can never run on a clone.  This task moves `init()`
into orchestrator thread setup (deliverable 4a) so each thread's engine — the
template for thread 0, a clone for threads 1..N in dynamic mode — gets its own
`init(thread_id, per_thread_connections)`.

A clone must reproduce steps 1–3 **in that order** so a cloned engine is
equivalent to the template *before* `init`; `init` then runs per thread on each
engine.  Re-exec of a stored path alone is insufficient (it skips helpers +
configure, and fails for NULL scripts).

## Deliverables

### 1. `src/scripting/script_api.h` — clone hook

```c
/* Return an independent engine equivalent to `src` AFTER create+
 * register_helpers+configure but BEFORE init(). Called once per worker thread
 * (thread 0 reuses the template). NULL ⇒ engine is not clonable; caller must
 * fall back to static mode. */
script_engine *(*clone)(script_engine *src);
```

### 2. Engine struct must retain enough to replay

`struct script_engine` currently holds only `{L, request_buf, request_cap}`.
Extend the Lua engine struct to retain the replay inputs captured at create/
configure/register time:

```c
struct script_engine {
    lua_State *L;
    char      *request_buf;
    size_t     request_cap;
    /* replay inputs for clone() */
    char      *path;          /* strdup of create() file, or NULL */
    char      *url;           /* strdup of configure() url, or NULL */
    char     **headers;       /* deep copy of configure() headers */
    size_t     n_headers;
    struct { char *ns; const script_helper *helpers; size_t count; } *helper_sets;
    size_t     n_helper_sets; /* recorded register_helpers() calls */
};
```

`lua_create`, `lua_configure`, and `lua_register_helpers` each record their
inputs into these fields.

### 3. `src/scripting/lua/engine.c` — `lua_clone`

```c
static script_engine *lua_clone(script_engine *src) {
    script_engine *e = lua_create(src->path);          /* step 1 (NULL-safe)   */
    if (!e) return NULL;
    for (size_t i = 0; i < src->n_helper_sets; i++)    /* step 2 (helpers)     */
        lua_register_helpers(e, src->helper_sets[i].ns,
                             src->helper_sets[i].helpers,
                             src->helper_sets[i].count);
    if (src->url || src->n_headers)                    /* step 3 (configure)   */
        lua_configure(e, src->url,
                      (const char *const *)src->headers, src->n_headers);
    return e;   /* init() runs later, per thread, in the orchestrator          */
}
```

Wire `lua_api.clone = lua_clone`.  Free the new replay fields in `lua_destroy`.

### 4. `src/orchestrator.c` — clone per thread + capability-driven dynamic

#### 4a. Move `init()` out of `main.c` into the orchestrator

Delete the `api->init(eng, 0, args.cfg.connections)` call at `main.c:306`.
main.c keeps `create` → helper registration → `configure`, then hands the
configured template to the orchestrator.  The orchestrator owns `init` because
only it knows the per-thread `thread_id` and per-thread connection split.

Lifecycle the orchestrator must enforce, in order:

1. **Template init (thread 0), during setup, before static pre-generation.**
   The static-request pre-gen at `orchestrator.c:718` calls `api->request()` on
   the template; in Lua, `request()` may depend on state set by `wrk.init`, so
   the template must be init'd first:
   ```c
   if (o->api && o->api->init)
       o->api->init(o->engine, 0, per_thread_conns);   /* thread 0 = template */
   ```
2. **Per-thread init for clones (dynamic mode only).**  In the thread loop, after
   `t->engine` is set: if this thread got its *own* clone (not the shared
   template), init that clone with its real `thread_id`:
   ```c
   if (t->engine != o->engine && o->api->init)
       o->api->init(t->engine, idx, t->connections);
   ```
   Thread 0 and all static-mode threads reuse the already-init'd template, so
   they are **not** re-init'd — this avoids calling `init` repeatedly on one
   shared engine (which would be wrong and, for a shared VM, racy).

#### 4b. Clone selection + dynamic flags

Add a `wants_response` flag to `othread` alongside `dynamic`. In the thread setup
loop (currently lines ~683–687):

```c
uint32_t caps = (o->api && o->api->capabilities)
              ? o->api->capabilities(o->engine) : 0;
bool dyn = (caps & SCRIPT_CAP_DYNAMIC_REQUEST) != 0;

if (idx == 0 || !dyn || !o->api->clone) {
    t->engine = o->engine;            /* thread 0, or static: reuse template  */
} else {
    t->engine = o->api->clone(o->engine);   /* isolated per-thread VM         */
    if (!t->engine) { t->engine = o->engine; dyn = false; } /* clone failed   */
}
t->dynamic       = dyn;
t->wants_response = dyn && (caps & SCRIPT_CAP_RESPONSE_HOOK) != 0;
```

Static path (`!dyn`) keeps using the once-generated `static_request_buf` and the
shared template engine exactly as today — zero behaviour change for HTTP.

Update `tell_response` to gate on `wants_response` instead of `dynamic`:

```c
if (t->wants_response && api && api->response && t->engine)
    api->response(t->engine, status, bytes, latency_us);
```

`ask_request` already gates on `t->dynamic` — correct as is.

The pre-spawn static-request generation (`orchestrator.c:718`) must be **skipped
when dynamic** (no shared-state call needed; each cloned engine generates its
own). Guard it: `if (!dynamic_workload && o->api->request) { ... }`.

### 5. `src/main.c` — stop initializing the engine

Remove the `api->init(eng, 0, args.cfg.connections)` line (`main.c:306`).  Leave
`create` → helper registration → `configure` in place.  `init` now belongs to the
orchestrator (4a).

### 6. `tests/unit/test_orchestrator.c` + `test_lua_engine.c`

```
test_clone_independent_state (engine)
  - script with a per-call counter; clone; advance original; clone unaffected
test_clone_replays_configure (engine)
  - configure template with custom host; clone; cloned request() uses same host
test_clone_replays_helpers (engine)
  - register a helper on template; clone; helper callable in clone
test_clone_null_script (engine)
  - create(NULL) → clone → produces default GET identical to template
test_dynamic_off_for_static_script (orchestrator)
  - static workload: all threads share template, t->dynamic == false
test_dynamic_on_clones_per_thread (orchestrator)
  - dynamic script: threads 1..N get distinct engine pointers
test_template_init_runs_once (orchestrator)
  - static workload: init() called exactly once, on the template, thread_id 0
test_clone_init_per_thread (orchestrator)
  - dynamic workload: each clone init'd once with its own thread_id;
    template not re-init'd
```

## Guards

- `make test` and `make test-asan` pass
- Static HTTP unchanged: `./wrkx -t4 -c100 -d3s -R100 http://localhost` same as before
- Dynamic multi-thread runs clean: a Lua script with `function request()` using
  `math.random()` under `-t4 -c20 -d3s -R50` — no crash, no ASAN error, distinct
  per-thread engines
- `include/wrkx_extension.h` and protocol layer git diffs empty

## Core engine touch

`script_api.h` gains the `clone` slot.  `orchestrator.c` gains per-thread clone,
two capability-driven flags, owns the `init()` lifecycle (moved from `main.c`),
and skips static pre-generation in dynamic mode.  `main.c` loses one line (the
`init` call).  This is the minimal orchestrator change Phase 5 genuinely needs,
isolated to this one task.  No scheduler/event-loop or protocol changes.
