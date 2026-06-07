title: QuickJS engine — response hook and clone for per-thread isolation
status: pending
adr: 0005
adr-step: P5-2
depends: t073

## Goal

Implement the remaining dynamic-mode hooks — `response()` and `clone()` — so the
QuickJS engine satisfies the full t069/t070 contract and runs safely across
threads.  After this task QuickJS supports the same multi-threaded dynamic
workloads as LuaJIT.

## Deliverables

### 1. `src/scripting/quickjs/engine.c` — `qjs_response`

```c
static void qjs_response(script_engine *se, int status,
                         size_t bytes, uint64_t latency_us) {
    qjs_engine *e = (qjs_engine *)se;
    JSValue fn = JS_GetPropertyStr(e->ctx, e->global, "response");
    if (!JS_IsFunction(e->ctx, fn)) { JS_FreeValue(e->ctx, fn); return; }
    JSValue a[3] = { JS_NewInt32(e->ctx, status),
                     JS_NewUint32(e->ctx, (uint32_t)bytes),
                     JS_NewFloat64(e->ctx, (double)latency_us / 1000.0) }; /* ms */
    JSValue r = JS_Call(e->ctx, fn, e->global, 3, a);
    if (JS_IsException(r)) dump_exception(e->ctx);
    JS_FreeValue(e->ctx, r);
    for (int i = 0; i < 3; i++) JS_FreeValue(e->ctx, a[i]);
    JS_FreeValue(e->ctx, fn);
}
```

### 2. `src/scripting/quickjs/engine.c` — `qjs_clone` with full replay

Match the t070 clone contract.
Replay order must match the template build order (create → register_helpers →
configure), same as `lua_clone` in t070.  Helper replay lands in t075; until then
the clone replays create + configure, with the helper step slotted **between**
them.  Each clone gets its own `JSRuntime`/`JSContext` — fully isolated, no
locking.

```c
static script_engine *qjs_clone(script_engine *src) {
    qjs_engine *s = (qjs_engine *)src;
    script_engine *e = qjs_create(s->path);            /* step 1 (NULL-safe)   */
    if (!e) return NULL;
    /* step 2 (register_helpers replay) inserted here in t075 */
    if (s->url || s->n_headers)                         /* step 3 (configure)  */
        qjs_configure(e, s->url,
                      (const char *const *)s->headers, s->n_headers);
    return e;
}
```

Wire `qjs_api.response` and `qjs_api.clone`.

### 3. `tests/unit/test_quickjs_engine.c` — new tests

```
test_response_calls_js_function        (last_status captured == 200)
test_response_missing_is_safe
test_response_exception_is_safe
test_clone_independent_request_state   (per-call counter not shared)
test_clone_replays_configure           (clone sees same wrk.host)
test_clone_destroy_independent         (destroy original; clone still usable)
test_clone_null_script                 (create(NULL) clone → default-equivalent)
```

### 4. ASAN clone lifecycle check

```sh
make test-asan        # QuickJS participates; verify clone create/destroy clean
```

## Guards

- All QuickJS unit tests pass; `make test-asan` clean on the clone path
- `./wrkx -t4 -c20 -d3s -R50 --engine=quickjs -s scripts/http_basic.js http://localhost`
  runs with four cloned engines, no crash, no ASAN error
- No orchestrator or protocol changes (clone/dynamic wiring already exists from t070)

## Core engine touch

Zero.  Confined to `src/scripting/quickjs/`.
