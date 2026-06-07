title: QuickJS engine — create, configure, init, destroy lifecycle
status: pending
adr: 0005
adr-step: P5-2
depends: t071

## Goal

Grow the QuickJS stub into a real engine that loads a JS file, accepts
`configure()` (URL/headers), calls an optional `init()` export, and tears down
deterministically.  Mirrors the LuaJIT engine's lifecycle and the t069 contract.
First unit tests for the QuickJS engine.

## Deliverables

### 1. `src/scripting/quickjs/engine.c` — struct + lifecycle

```c
typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue    global;
    /* replay inputs for clone() in t074 — recorded here, used later */
    char      *path;
    char      *url;
    char     **headers;
    size_t     n_headers;
} qjs_engine;
```

**`qjs_create(file)`**
1. `JS_NewRuntime()`; set memory limit (default 64 MiB, override `QJS_MEMORY_MB`)
   via `JS_SetMemoryLimit`.
2. `JS_NewContext(rt)`.
3. `JS_AddIntrinsicBaseObjects` + `js_std_add_helpers` (gives `console.log`).
4. Read `file` (NULL ⇒ no script; default behaviour set up by `configure`).
   `JS_Eval(ctx, buf, len, file, JS_EVAL_TYPE_GLOBAL)`.
5. On exception: `dump_exception(ctx)`, free, return NULL.
6. `e->global = JS_GetGlobalObject(ctx)`; `strdup` path.

**`qjs_configure(engine, url, headers, n)`** — language-neutral equivalent of
`lua_configure`.  Parse `url` (scheme/host/port/path) and expose to JS as a
`wrk` global object so JS scripts read the target the same way Lua scripts read
`wrk.host` etc.:

```c
// JS sees: wrk.scheme, wrk.host, wrk.port, wrk.path, wrk.headers (array)
```

Record `url`/`headers` into the struct for clone replay.  Return 0 on success.

**`qjs_init(engine, thread_id, connections)`**
```c
JSValue fn = JS_GetPropertyStr(ctx, e->global, "init");
if (JS_IsFunction(ctx, fn)) {
    JSValue a[2] = { JS_NewUint32(ctx, thread_id),
                     JS_NewUint32(ctx, connections) };
    JSValue r = JS_Call(ctx, fn, e->global, 2, a);
    if (JS_IsException(r)) dump_exception(ctx);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]);
}
JS_FreeValue(ctx, fn);
```

**`qjs_destroy`** — free global, headers, url, path, context, runtime, struct.

**`dump_exception`** helper — `JS_GetException` → `JS_ToCString` → stderr → free.

Wire `qjs_api`: `name`, `create`, `configure`, `init`, `destroy` (request/
response/capabilities/register_helpers/clone added in t073–t075).

### 2. `tests/unit/test_quickjs_engine.c`

```
test_create_empty_script_succeeds      (empty file → non-NULL; destroy clean)
test_create_syntax_error_returns_null  ("function (" → NULL, no crash)
test_create_null_file_succeeds         (create(NULL) → non-NULL)
test_configure_exposes_wrk_globals     (configure host=example.com →
                                        eval "wrk.host" == "example.com")
test_init_calls_js_function            (var x=0; function init(){x=1} → x==1)
test_init_missing_function_is_safe     (no init export → no crash)
test_init_exception_is_safe            (init throws → no crash, no abort)
```

Gate the test binary on `QUICKJS_ENABLED=1`; print `SKIP — QuickJS not enabled`
and exit 0 otherwise so `make test` stays green without the flag.

## Guards

- `./configure --with-quickjs && make test` — new tests pass
- `make test-asan` (with QuickJS enabled) — clean; QuickJS uses system malloc, no
  ASAN conflict (unlike LuaJIT)
- No LuaJIT, orchestrator, or protocol files changed

## Core engine touch

Zero.  New code confined to `src/scripting/quickjs/` and its unit test.
