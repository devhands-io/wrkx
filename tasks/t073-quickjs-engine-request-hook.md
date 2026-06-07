title: QuickJS engine — request hook, capabilities, and HTTP E2E
status: pending
adr: 0005
adr-step: P5-2
depends: t072

## Goal

Implement `request()` and `capabilities()` for the QuickJS engine, plus an
`--engine` selector so wrkx can drive a JS HTTP workload end to end.

## Deliverables

### 1. `src/scripting/quickjs/engine.c` — `qjs_request`

```c
static char *qjs_request(script_engine *se, size_t *len_out) {
    qjs_engine *e = (qjs_engine *)se;
    JSValue fn = JS_GetPropertyStr(e->ctx, e->global, "request");
    if (!JS_IsFunction(e->ctx, fn)) {
        JS_FreeValue(e->ctx, fn); *len_out = 0; return NULL;
    }
    JSValue ret = JS_Call(e->ctx, fn, e->global, 0, NULL);
    JS_FreeValue(e->ctx, fn);
    if (JS_IsException(ret)) {
        dump_exception(e->ctx); JS_FreeValue(e->ctx, ret);
        *len_out = 0; return NULL;
    }
    size_t slen; const char *s = JS_ToCStringLen(e->ctx, &slen, ret);
    char *out = NULL;
    if (s) { out = malloc(slen); memcpy(out, s, slen); *len_out = slen;
             JS_FreeCString(e->ctx, s); }
    JS_FreeValue(e->ctx, ret);
    return out;   /* caller frees */
}
```

### 2. `src/scripting/quickjs/engine.c` — `qjs_capabilities`

Language-neutral capability detection (mirror of the Lua contract from t069):
```c
static uint32_t qjs_capabilities(script_engine *se) {
    qjs_engine *e = (qjs_engine *)se;
    uint32_t caps = 0;
    JSValue req = JS_GetPropertyStr(e->ctx, e->global, "request");
    if (JS_IsFunction(e->ctx, req))  caps |= SCRIPT_CAP_DYNAMIC_REQUEST;
    JS_FreeValue(e->ctx, req);
    JSValue rsp = JS_GetPropertyStr(e->ctx, e->global, "response");
    if (JS_IsFunction(e->ctx, rsp)) caps |= SCRIPT_CAP_RESPONSE_HOOK;
    JS_FreeValue(e->ctx, rsp);
    return caps;
}
```

(JS has no `wrk.lua` default, so "request is a function" genuinely means the user
wrote per-request generation — no default-comparison dance needed, unlike Lua.)

Wire `qjs_api.request` and `qjs_api.capabilities`.

### 3. `src/cli.h` + cli parser — `--engine` selector

Add `-E <name>` / `--engine=<name>` (values: `lua` default, `quickjs`).  Store on
`cli_args`.  This is the only core-adjacent touch and it lives in CLI parsing.

### 4. `src/main.c` — engine factory switch

```c
script_api *api;
if (args.engine && strcmp(args.engine, "quickjs") == 0) {
#if WRKX_HAVE_QUICKJS
    api = quickjs_script_api();
#else
    fprintf(stderr, "wrkx built without QuickJS (./configure --with-quickjs)\n");
    return 1;
#endif
} else {
    api = lua_script_api();
}
```

The orchestrator startup sequence is unchanged — it still receives a
`script_api *` + `script_engine *` from main.c.

### 5. `scripts/http_basic.js`

```js
// Minimal HTTP/1.1 GET workload (QuickJS engine).
// ./wrkx -t2 -c10 -d5s -R100 --engine=quickjs -s scripts/http_basic.js http://localhost
function request() {
    return "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
}
```

### 6. `tests/unit/test_quickjs_engine.c` — new tests

```
test_request_returns_exact_bytes
test_request_state_persists_across_calls   (var n=0; n++ each call)
test_request_exception_returns_null
test_request_missing_returns_null
test_capabilities_request_only_is_dynamic
test_capabilities_with_response_sets_both_bits
test_capabilities_empty_script_is_zero
```

### 7. `tests/e2e/quickjs_http.sh`

Reuse whatever HTTP mock/loopback the Lua HTTP E2E uses.  Run:
```sh
./wrkx -t2 -c10 -d3s -R50 --engine=quickjs -s scripts/http_basic.js http://127.0.0.1:PORT
```
Assert exit 0 and `Requests/sec` > 0.  `SKIP` with a clear message if QuickJS is
not built or no mock server is available.

## Guards

- `make test` (with QuickJS) — unit + E2E green
- `make test-asan` — clean
- `./wrkx --engine=quickjs -t1 -c1 -d2s -R10 -s scripts/http_basic.js http://localhost`
  exits 0
- Lua path unaffected: omitting `--engine` behaves exactly as before

## Core engine touch

One new CLI flag (`cli.h` + parser) and a factory `switch` in `main.c`.
Orchestrator, scheduler, and protocol layers unchanged.
