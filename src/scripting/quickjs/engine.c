/* src/scripting/quickjs/engine.c
 *
 * QuickJS Request-Layer engine (ADR 0005, Phase 5, t072).
 *
 * Lifecycle: create, configure, init, destroy.  Request/response/clone and
 * helper registration are added in t073-t075.  Only the QuickJS vendored
 * library and the project-internal http_parser are included; no protocol
 * headers (Invariant 3).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "scripting/quickjs/engine.h"
#include "quickjs.h"
#include "http_parser.h"

#define QJS_DEFAULT_MEMORY_MB 64

/*
 * Per-call context bundle passed through the void * engine_ctx ABI to
 * QuickJS-shaped helper functions (script_helper_fn).
 *
 * LAYOUT CONTRACT: must match qjs_helper_ctx in
 * extensions/redis/redis_quickjs_helpers.h exactly.  Both files include
 * quickjs.h, so JSContext/JSValueConst/JSValue resolve to the same ABI types.
 * If you change this definition, change the one in redis_quickjs_helpers.h too.
 */
typedef struct {
    JSContext    *ctx;
    int           argc;
    JSValueConst *argv;   /* JS arguments (borrowed for the duration of the call) */
    JSValue       ret;    /* helper sets this to the JS return value              */
} qjs_helper_ctx;

/* One recorded register_helpers() call — stored for clone() replay. */
typedef struct {
    char                *ns;       /* strdup'd namespace (e.g. "redis") */
    const script_helper *helpers;  /* points to static table; not owned  */
    size_t               count;
} qjs_hrec;

typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue    global;
    /* replay inputs for clone() */
    char      *path;
    char      *url;
    char     **headers;
    size_t     n_headers;
    qjs_hrec  *hrecs;      /* recorded register_helpers() calls */
    size_t     n_hrecs;
} qjs_engine;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void dump_exception(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    if (str) {
        fprintf(stderr, "quickjs: %s\n", str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, exc);
}

/* -------------------------------------------------------------------------
 * script_api vtable
 * ---------------------------------------------------------------------- */

static script_engine *qjs_create(const char *file) {
    qjs_engine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->rt = JS_NewRuntime();
    if (!e->rt) { free(e); return NULL; }

    const char *mem_env = getenv("QJS_MEMORY_MB");
    size_t mem_mb = (mem_env && *mem_env)
                    ? (size_t) strtoul(mem_env, NULL, 10)
                    : QJS_DEFAULT_MEMORY_MB;
    JS_SetMemoryLimit(e->rt, mem_mb * 1024 * 1024);

    e->ctx = JS_NewContext(e->rt);
    if (!e->ctx) { JS_FreeRuntime(e->rt); free(e); return NULL; }
    /* JS_NewContext already installs all standard intrinsics; nothing else needed. */

    if (file != NULL) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            fprintf(stderr, "quickjs: cannot open %s\n", file);
            goto fail_ctx;
        }
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        rewind(f);

        char *buf = malloc((size_t) flen + 1);
        if (!buf) { fclose(f); goto fail_ctx; }

        if (fread(buf, 1, (size_t) flen, f) != (size_t) flen) {
            free(buf); fclose(f); goto fail_ctx;
        }
        fclose(f);
        buf[flen] = '\0';

        JSValue result = JS_Eval(e->ctx, buf, (size_t) flen, file,
                                 JS_EVAL_TYPE_GLOBAL);
        free(buf);

        if (JS_IsException(result)) {
            dump_exception(e->ctx);
            JS_FreeValue(e->ctx, result);
            goto fail_ctx;
        }
        JS_FreeValue(e->ctx, result);
        e->path = strdup(file);
    }

    e->global = JS_GetGlobalObject(e->ctx);
    return (script_engine *) e;

fail_ctx:
    JS_FreeContext(e->ctx);
    JS_FreeRuntime(e->rt);
    free(e);
    return NULL;
}

static int qjs_configure(script_engine *se, const char *url,
                          const char * const *headers, size_t n_headers) {
    if (!se) return -1;
    qjs_engine *e = (qjs_engine *) se;
    JSContext  *ctx = e->ctx;

    /* Record for clone() replay (t074). */
    free(e->url);
    e->url = url ? strdup(url) : NULL;
    for (size_t i = 0; i < e->n_headers; i++) free(e->headers[i]);
    free(e->headers);
    e->headers   = NULL;
    e->n_headers = 0;
    if (n_headers > 0 && headers) {
        e->headers = calloc(n_headers, sizeof(char *));
        if (e->headers) {
            for (size_t i = 0; i < n_headers; i++) {
                e->headers[i] = headers[i] ? strdup(headers[i]) : NULL;
                e->n_headers++;
            }
        }
    }

    /* Build JS `wrk` object: wrk.scheme, .host, .port, .path, .headers */
    JSValue wrk = JS_NewObject(ctx);

    if (url) {
        struct http_parser_url u;
        memset(&u, 0, sizeof(u));
        if (http_parser_parse_url(url, strlen(url), 0, &u) == 0) {
            if (u.field_set & (1 << UF_SCHEMA))
                JS_SetPropertyStr(ctx, wrk, "scheme",
                    JS_NewStringLen(ctx, url + u.field_data[UF_SCHEMA].off,
                                   u.field_data[UF_SCHEMA].len));
            if (u.field_set & (1 << UF_HOST))
                JS_SetPropertyStr(ctx, wrk, "host",
                    JS_NewStringLen(ctx, url + u.field_data[UF_HOST].off,
                                   u.field_data[UF_HOST].len));
            if (u.field_set & (1 << UF_PORT))
                JS_SetPropertyStr(ctx, wrk, "port",
                    JS_NewUint32(ctx, u.port));
            if (u.field_set & (1 << UF_PATH))
                JS_SetPropertyStr(ctx, wrk, "path",
                    JS_NewStringLen(ctx, url + u.field_data[UF_PATH].off,
                                   u.field_data[UF_PATH].len));
        }
    }

    /* wrk.headers: object mapping "Key" → "value" (mirrors Lua shape). */
    if (headers && n_headers > 0) {
        JSValue hdrs = JS_NewObject(ctx);
        for (size_t i = 0; i < n_headers; i++) {
            if (!headers[i]) continue;
            const char *colon = strchr(headers[i], ':');
            if (!colon || colon[1] != ' ') continue;
            size_t klen = (size_t)(colon - headers[i]);
            /* Stack-allocate a NUL-terminated key; fall back to heap for long keys. */
            char kbuf[128];
            char *key = (klen < sizeof(kbuf)) ? kbuf : malloc(klen + 1);
            if (!key) continue;
            memcpy(key, headers[i], klen);
            key[klen] = '\0';
            JS_SetPropertyStr(ctx, hdrs, key, JS_NewString(ctx, colon + 2));
            if (key != kbuf) free(key);
        }
        JS_SetPropertyStr(ctx, wrk, "headers", hdrs);
    }

    JS_SetPropertyStr(ctx, e->global, "wrk", wrk);
    return 0;
}

static void qjs_init(script_engine *se, uint64_t thread_id,
                     uint64_t connections) {
    if (!se) return;
    qjs_engine *e = (qjs_engine *) se;
    JSContext  *ctx = e->ctx;

    JSValue fn = JS_GetPropertyStr(ctx, e->global, "init");
    if (JS_IsFunction(ctx, fn)) {
        JSValue args[2] = {
            JS_NewUint32(ctx, (uint32_t) thread_id),
            JS_NewUint32(ctx, (uint32_t) connections),
        };
        JSValue r = JS_Call(ctx, fn, e->global, 2, args);
        if (JS_IsException(r)) dump_exception(ctx);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    }
    JS_FreeValue(ctx, fn);
}

static uint32_t qjs_capabilities(script_engine *se) {
    if (!se) return 0;
    qjs_engine *e = (qjs_engine *) se;
    JSContext  *ctx = e->ctx;
    uint32_t caps = 0;

    JSValue req = JS_GetPropertyStr(ctx, e->global, "request");
    if (JS_IsFunction(ctx, req)) caps |= SCRIPT_CAP_DYNAMIC_REQUEST;
    JS_FreeValue(ctx, req);

    JSValue rsp = JS_GetPropertyStr(ctx, e->global, "response");
    if (JS_IsFunction(ctx, rsp)) caps |= SCRIPT_CAP_RESPONSE_HOOK;
    JS_FreeValue(ctx, rsp);

    return caps;
}

static char *qjs_request(script_engine *se, size_t *len_out) {
    if (len_out) *len_out = 0;
    if (!se) return NULL;
    qjs_engine *e = (qjs_engine *) se;
    JSContext  *ctx = e->ctx;

    JSValue fn = JS_GetPropertyStr(ctx, e->global, "request");
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return NULL;
    }
    JSValue ret = JS_Call(ctx, fn, e->global, 0, NULL);
    JS_FreeValue(ctx, fn);

    if (JS_IsException(ret)) {
        dump_exception(ctx);
        JS_FreeValue(ctx, ret);
        return NULL;
    }

    size_t slen = 0;
    const char *s = JS_ToCStringLen(ctx, &slen, ret);
    char *out = NULL;
    if (s && slen > 0) {
        out = malloc(slen + 1);   /* +1 for NUL so callers may treat as C string */
        if (out) {
            memcpy(out, s, slen);
            out[slen] = '\0';
            if (len_out) *len_out = slen;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, ret);
    return out;   /* caller frees */
}

static void qjs_response(script_engine *se, int status,
                          size_t bytes, uint64_t latency_us) {
    if (!se) return;
    qjs_engine *e = (qjs_engine *) se;
    JSContext  *ctx = e->ctx;

    JSValue fn = JS_GetPropertyStr(ctx, e->global, "response");
    if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return; }

    JSValue args[3] = {
        JS_NewInt32(ctx,  status),
        JS_NewUint32(ctx, (uint32_t) bytes),
        JS_NewFloat64(ctx, (double) latency_us / 1000.0),  /* µs → ms */
    };
    JSValue r = JS_Call(ctx, fn, e->global, 3, args);
    if (JS_IsException(r)) dump_exception(ctx);
    JS_FreeValue(ctx, r);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, fn);
}

/* -------------------------------------------------------------------------
 * Helper registration (t075)
 * ---------------------------------------------------------------------- */

/*
 * Trampoline: called by the JS engine when a registered helper is invoked.
 * func_data[0] carries the raw bytes of the script_helper_fn pointer boxed
 * as an ArrayBuffer so no precision is lost regardless of pointer width.
 */
static JSValue js_helper_trampoline(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    int magic, JSValue *func_data) {
    (void) this_val; (void) magic;

    /* Unwrap the function pointer. */
    size_t   blen = 0;
    uint8_t *bp   = JS_GetArrayBuffer(ctx, &blen, func_data[0]);
    if (!bp || blen != sizeof(script_helper_fn)) return JS_EXCEPTION;

    script_helper_fn fn;
    memcpy(&fn, bp, sizeof fn);

    qjs_helper_ctx h = {
        .ctx  = ctx,
        .argc = argc,
        .argv = argv,
        .ret  = JS_UNDEFINED,
    };
    int rc = fn(&h);
    if (rc != 0 && JS_IsUndefined(h.ret)) return JS_EXCEPTION;
    return h.ret;
}

static void qjs_register_helpers(script_engine *se, const char *ns,
                                  const script_helper *helpers, size_t count) {
    if (!se || !ns || !helpers || count == 0) return;
    qjs_engine *e   = (qjs_engine *) se;
    JSContext  *ctx = e->ctx;

    /* Build a JS object with one property per helper. */
    JSValue obj = JS_NewObject(ctx);
    for (size_t i = 0; i < count; i++) {
        script_helper_fn fn = helpers[i].fn;
        /* Box the raw pointer as an ArrayBuffer so float64 precision is not
         * an issue on 64-bit targets with high-address code segments. */
        JSValue carry = JS_NewArrayBufferCopy(ctx,
                            (const uint8_t *) &fn, sizeof fn);
        JSValue jsfn  = JS_NewCFunctionData(ctx, js_helper_trampoline,
                                            /*length*/ 0, /*magic*/ 0,
                                            /*data_len*/ 1, &carry);
        JS_FreeValue(ctx, carry);
        JS_SetPropertyStr(ctx, obj, helpers[i].name, jsfn);
    }
    JS_SetPropertyStr(ctx, e->global, ns, obj);

    /* Record for clone() replay. */
    qjs_hrec *nr = realloc(e->hrecs, (e->n_hrecs + 1) * sizeof *nr);
    if (nr) {
        e->hrecs = nr;
        e->hrecs[e->n_hrecs].ns      = strdup(ns);
        e->hrecs[e->n_hrecs].helpers = helpers;
        e->hrecs[e->n_hrecs].count   = count;
        e->n_hrecs++;
    }
}

static script_engine *qjs_clone(script_engine *src) {
    if (!src) return NULL;
    qjs_engine *s = (qjs_engine *) src;

    script_engine *e = qjs_create(s->path);   /* step 1: fresh runtime + script */
    if (!e) return NULL;

    /* step 2: replay register_helpers (before configure, matching the
     * create → register_helpers → configure order used for the template). */
    for (size_t i = 0; i < s->n_hrecs; i++)
        qjs_register_helpers(e, s->hrecs[i].ns,
                             s->hrecs[i].helpers, s->hrecs[i].count);

    /* step 3: configure replay */
    if (s->url || s->n_headers)
        qjs_configure(e, s->url,
                      (const char * const *) s->headers, s->n_headers);
    return e;
}

static void qjs_destroy(script_engine *se) {
    if (!se) return;
    qjs_engine *e = (qjs_engine *) se;
    if (e->ctx) {
        JS_FreeValue(e->ctx, e->global);
        JS_FreeContext(e->ctx);
    }
    if (e->rt) JS_FreeRuntime(e->rt);
    free(e->path);
    free(e->url);
    for (size_t i = 0; i < e->n_headers; i++) free(e->headers[i]);
    free(e->headers);
    for (size_t i = 0; i < e->n_hrecs; i++) free(e->hrecs[i].ns);
    free(e->hrecs);
    free(e);
}

/* -------------------------------------------------------------------------
 * Engine introspection (for unit tests and glue modules)
 * ---------------------------------------------------------------------- */

void *qjs_engine_ctx(script_engine *se) {
    return se ? ((qjs_engine *) se)->ctx : NULL;
}

void *qjs_engine_global(script_engine *se) {
    return se ? &((qjs_engine *) se)->global : NULL;
}

/* -------------------------------------------------------------------------
 * vtable
 * ---------------------------------------------------------------------- */

static script_api qjs_api = {
    .name             = "quickjs",
    .create           = qjs_create,
    .configure        = qjs_configure,
    .capabilities     = qjs_capabilities,
    .register_helpers = qjs_register_helpers,
    .clone            = qjs_clone,
    .init             = qjs_init,
    .request          = qjs_request,
    .response         = qjs_response,
    .destroy          = qjs_destroy,
    /* .done: t076 */
};

script_api *quickjs_script_api(void) {
    return &qjs_api;
}
