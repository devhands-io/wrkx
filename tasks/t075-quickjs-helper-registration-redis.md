title: QuickJS — vtable helper registration and Redis bindings
status: pending
adr: 0005
adr-step: P5-3
depends: t074

## Goal

Implement the `register_helpers` vtable slot (added to the contract in t069) for
QuickJS, so extension glue can expose C helpers as a JS namespace.  Then bind the
existing Redis **encoder** (`redis_make_request()`) — no Redis protocol code
duplicated — while providing a QuickJS-native helper table.

## ABI reality (from the t069 decision)

The helper ABI is **not** reusable across engines.  `script_helper_fn` is
`int(void *engine_ctx)` and the existing `redis_lua_helpers[]` bodies cast
`engine_ctx` to `lua_State *` and drive the Lua stack
(`extensions/redis/redis_lua_helpers.c:29`).  QuickJS cannot execute those.

Per t069's recorded decision, the ABI stays frozen and helpers are
**engine-specific**.  This task therefore adds a *new* `redis_quickjs_helpers[]`
table; it does **not** reuse `redis_lua_helpers[]`.  The only shared code is the
engine-neutral encoder `redis_make_request()` (already in `extensions/redis/`),
so the Redis protocol layer is untouched.

`engine_ctx` for QuickJS is a per-call bundle:

```c
typedef struct {
    JSContext      *ctx;
    int             argc;
    JSValueConst   *argv;   /* JS call arguments */
    JSValue         ret;    /* helper writes its result here */
} qjs_helper_ctx;
```

### Engine-specific Redis helper — `extensions/redis/redis_quickjs_helpers.c`

```c
/* int(void *engine_ctx) — same ABI as the Lua helper, QuickJS-shaped body. */
static int qjs_redis_command(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) { /* set h->ret to a thrown JS error */ ... return -1; }
    const char *argv[REDIS_MAX_ARGS]; size_t arglens[REDIS_MAX_ARGS];
    for (int i = 0; i < h->argc; i++)
        argv[i] = JS_ToCStringLen(h->ctx, &arglens[i], h->argv[i]);
    size_t len; char *buf = redis_make_request(h->argc, argv, arglens, &len);
    /* free the JS C-strings */
    h->ret = buf ? JS_NewStringLen(h->ctx, buf, len) : JS_EXCEPTION;
    free(buf);
    return buf ? 0 : -1;
}
extern const script_helper redis_quickjs_helpers[];   /* in the .h */
```

`extensions/redis/init.c` registers **both** tables under engine-tagged
namespaces (the t069 convention — the extension API has no engine name, so the
tag rides in the namespace string):

```c
api->register_helpers("redis@lua",     redis_lua_helpers,     redis_lua_helpers_count);
api->register_helpers("redis@quickjs", redis_quickjs_helpers, redis_quickjs_helpers_count);
```

The **host** (`main.c`, logic added in t069) binds only the set whose `@engine`
suffix matches the active engine's `api->name`, stripping the suffix so both
engines see the bare `redis` namespace.  No `wrkx_extension.h` change.

> Migration note: t069 already retags the existing Lua table as
> `register_helpers("redis@lua", ...)` and does the same for other Lua-shaped
> extension helpers such as memcached. This task adds the QuickJS companion table
> and updates Redis init to register both tagged variants.

## Deliverables

### 1. `src/scripting/quickjs/engine.c` — `qjs_register_helpers`

Create a JS object for the namespace; attach each `script_helper` as a JS C
function whose trampoline builds a `qjs_helper_ctx` from the live JS call, invokes
`helper.fn(&ctx)` (the `int(void*)` ABI), then returns `ctx.ret`:

```c
/* The script_helper_fn pointer is carried in func_data via JS_NewCFunctionData. */
static JSValue js_helper_trampoline(JSContext *ctx, JSValueConst this_val,
        int argc, JSValueConst *argv, int magic, JSValue *func_data) {
    script_helper_fn fn = /* unwrap pointer carried in func_data[0] */;
    qjs_helper_ctx h = { .ctx = ctx, .argc = argc, .argv = argv,
                         .ret = JS_UNDEFINED };
    int rc = fn(&h);                 /* engine_ctx == &h (the bundle) */
    if (rc != 0 && JS_IsUndefined(h.ret)) return JS_EXCEPTION;
    return h.ret;                    /* already a JS string or JS_EXCEPTION */
}

static void qjs_register_helpers(script_engine *se, const char *ns,
                                  const script_helper *helpers, size_t count) {
    qjs_engine *e = (qjs_engine *)se;
    JSValue obj = JS_NewObject(e->ctx);
    for (size_t i = 0; i < count; i++) {
        /* carry helpers[i].fn as opaque data (e.g. a boxed pointer) */
        JSValue carry = /* box (void*)helpers[i].fn into a JSValue */;
        JSValue fn = JS_NewCFunctionData(e->ctx, js_helper_trampoline,
                                         /*length*/0, /*magic*/0, 1, &carry);
        JS_SetPropertyStr(e->ctx, obj, helpers[i].name, fn);
    }
    JS_SetPropertyStr(e->ctx, e->global, ns, obj);
    /* record (ns,helpers,count) into the struct for clone replay */
}
```

Note: only the `script_helper_fn` pointer is carried — there is no separate
"engine ptr" argument, because the ABI passes everything through the single
`engine_ctx` (here `&h`).  Wire `qjs_api.register_helpers = qjs_register_helpers`.

### 2. `src/scripting/quickjs/engine.c` — complete clone helper replay

Finish the t074 clone TODO: replay recorded `register_helpers` calls **between
`qjs_create` and `qjs_configure`** (the create → register_helpers → configure
order shared with `lua_clone`), so cloned engines expose the same namespaces as
the template.

### 3. `scripts/redis_get_set.js`

```js
// Equivalent of scripts/redis_get_set.lua.
// ./wrkx -t4 -c100 -d10s -R1000 --engine=quickjs -s scripts/redis_get_set.js redis://localhost:6379
var counter = 0;
function request() {
    counter++;
    var key = "wrkx:key:" + (counter % 100);
    return (counter % 2 === 0) ? redis.set(key, "value") : redis.get(key);
}
```

### 4. `tests/unit/test_quickjs_redis.c`

```
test_redis_namespace_registered     (typeof redis === "object")
test_redis_get_is_function          (typeof redis.get === "function")
test_redis_get_returns_command_bytes
test_redis_set_returns_command_bytes
test_clone_replays_redis_helpers    (clone has redis.* available)
```

### 5. `tests/e2e/quickjs_redis_mock.sh`

Use the existing Redis mock (`tests/e2e/redis_mock_server.py` if present, else the
mock the Lua Redis E2E uses).  Run the JS workload 3s, assert `Requests/sec` > 0,
exit 0.  `SKIP` cleanly if QuickJS not built or no mock available.

## Guards

- `make test` (with QuickJS) — unit + E2E green
- `make test-asan` — clean through the helper trampoline + clone replay
- `tests/e2e/quickjs_redis_mock.sh` exits 0
- Redis **protocol** layer (`extensions/redis/redis.c` + parser + `redis_make_request`)
  git diff empty — only the new `redis_quickjs_helpers.{c,h}` table, the
  tagged Redis helper registrations in `init.c`, and the QuickJS glue are added
- `include/wrkx_extension.h` git diff empty (`script_helper_fn` ABI frozen)

## Core engine touch

Zero core-engine change.  New engine-specific helper table
(`redis_quickjs_helpers[]`) sharing only the existing `redis_make_request()`
encoder, plus the QuickJS `register_helpers` glue.  The vtable slot it fills was
added in t069; the frozen-ABI / engine-specific-table decision was made there.
