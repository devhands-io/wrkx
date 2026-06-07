/*
 * Redis QuickJS glue module (ADR 0005, Phase 5, t075).
 *
 * Exposes redis.command(), redis.get(), and redis.set() under the "redis"
 * JS namespace.  Registered by init.c via api->register_helpers("redis@quickjs").
 *
 * The shared encoder redis_make_request() (extensions/redis/redis.c) handles
 * all RESP framing — no Redis protocol code is duplicated here.
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/quickjs/ headers, standard library.  NO src/ headers.
 */

#ifdef WRKX_HAVE_QUICKJS

#include "redis_quickjs_helpers.h"
#include "redis.h"    /* redis_make_request() */

#include <stdlib.h>
#include <string.h>

#define REDIS_MAX_ARGS 64

/* -------------------------------------------------------------------------
 * redis.command(cmd, arg, ...) -> binary RESP string
 *
 * Encodes an arbitrary Redis command; the result is a heap RESP frame ready
 * to be returned from request() and written to the wire.
 * ---------------------------------------------------------------------- */

static int qjs_redis_command(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *) engine_ctx;

    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
            "redis.command: at least one argument required");
        return -1;
    }
    if (h->argc > REDIS_MAX_ARGS) {
        h->ret = JS_ThrowRangeError(h->ctx,
            "redis.command: too many arguments (max %d)", REDIS_MAX_ARGS);
        return -1;
    }

    const char *argv[REDIS_MAX_ARGS];
    size_t      arglens[REDIS_MAX_ARGS];
    int         nconv = 0;

    for (int i = 0; i < h->argc; i++) {
        argv[i] = JS_ToCStringLen(h->ctx, &arglens[i], h->argv[i]);
        if (!argv[i]) {
            for (int j = 0; j < nconv; j++) JS_FreeCString(h->ctx, argv[j]);
            h->ret = JS_ThrowTypeError(h->ctx,
                "redis.command: argument %d is not a string", i + 1);
            return -1;
        }
        nconv++;
    }

    size_t len = 0;
    char  *buf = redis_make_request(h->argc, argv, arglens, &len);
    for (int i = 0; i < nconv; i++) JS_FreeCString(h->ctx, argv[i]);

    if (!buf) {
        h->ret = JS_ThrowInternalError(h->ctx,
            "redis.command: failed to encode RESP command");
        return -1;
    }

    h->ret = JS_NewStringLen(h->ctx, buf, len);
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * redis.get(key) -> binary RESP string
 * ---------------------------------------------------------------------- */

static int qjs_redis_get(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *) engine_ctx;

    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx, "redis.get: key argument required");
        return -1;
    }

    size_t      klen;
    const char *key = JS_ToCStringLen(h->ctx, &klen, h->argv[0]);
    if (!key) { h->ret = JS_EXCEPTION; return -1; }

    const char *argv[]   = { "GET", key };
    size_t      alens[]  = { 3,     klen };
    size_t len = 0;
    char  *buf = redis_make_request(2, argv, alens, &len);
    JS_FreeCString(h->ctx, key);

    if (!buf) {
        h->ret = JS_ThrowInternalError(h->ctx,
            "redis.get: failed to encode GET command");
        return -1;
    }

    h->ret = JS_NewStringLen(h->ctx, buf, len);
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * redis.set(key, value) -> binary RESP string
 * ---------------------------------------------------------------------- */

static int qjs_redis_set(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *) engine_ctx;

    if (h->argc < 2) {
        h->ret = JS_ThrowTypeError(h->ctx,
            "redis.set: key and value arguments required");
        return -1;
    }

    size_t      klen, vlen;
    const char *key = JS_ToCStringLen(h->ctx, &klen, h->argv[0]);
    if (!key) { h->ret = JS_EXCEPTION; return -1; }

    const char *val = JS_ToCStringLen(h->ctx, &vlen, h->argv[1]);
    if (!val) {
        JS_FreeCString(h->ctx, key);
        h->ret = JS_EXCEPTION;
        return -1;
    }

    const char *argv[]  = { "SET", key, val };
    size_t      alens[] = { 3,     klen, vlen };
    size_t len = 0;
    char  *buf = redis_make_request(3, argv, alens, &len);
    JS_FreeCString(h->ctx, key);
    JS_FreeCString(h->ctx, val);

    if (!buf) {
        h->ret = JS_ThrowInternalError(h->ctx,
            "redis.set: failed to encode SET command");
        return -1;
    }

    h->ret = JS_NewStringLen(h->ctx, buf, len);
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public helper table
 * ---------------------------------------------------------------------- */

const script_helper redis_quickjs_helpers[] = {
    { "command", qjs_redis_command },
    { "get",     qjs_redis_get     },
    { "set",     qjs_redis_set     },
};

const size_t redis_quickjs_helpers_count =
    sizeof(redis_quickjs_helpers) / sizeof(redis_quickjs_helpers[0]);

#endif /* WRKX_HAVE_QUICKJS */
