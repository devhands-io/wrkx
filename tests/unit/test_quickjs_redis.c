/* tests/unit/test_quickjs_redis.c
 *
 * Unit tests for the QuickJS Redis helper registration (ADR 0005, t075):
 *   register_helpers / redis namespace / clone replay
 *
 * Gated on WRKX_HAVE_QUICKJS: if the binary is compiled without QuickJS
 * support the main() below prints "SKIP" and exits 0 so make test stays green.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WRKX_HAVE_QUICKJS

int main(void) {
    puts("SKIP — QuickJS not enabled");
    return 0;
}

#else /* WRKX_HAVE_QUICKJS */

#include "unity.h"
#include "quickjs.h"
#include "scripting/script_api.h"
#include "scripting/quickjs/engine.h"

/* The QuickJS Redis helper table under test. */
#include "redis_quickjs_helpers.h"

void setUp(void) {}
void tearDown(void) {}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Evaluate a JS expression, return heap copy of the string result (or NULL). */
static char *eval_str(script_engine *e, const char *js) {
    JSContext *ctx = (JSContext *) qjs_engine_ctx(e);
    JSValue v = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        if (s) { fprintf(stderr, "JS exception: %s\n", s); JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, v);
        return NULL;
    }
    const char *cs = JS_ToCString(ctx, v);
    char *out = cs ? strdup(cs) : NULL;
    JS_FreeCString(ctx, cs);
    JS_FreeValue(ctx, v);
    return out;
}

/* Register the redis_quickjs_helpers table under "redis" into engine *e. */
static void bind_redis(script_engine *e) {
    script_api *api = quickjs_script_api();
    TEST_ASSERT_NOT_NULL(api->register_helpers);
    api->register_helpers(e, "redis",
                          redis_quickjs_helpers,
                          redis_quickjs_helpers_count);
}

/* -------------------------------------------------------------------------
 * test_redis_namespace_registered
 * ---------------------------------------------------------------------- */

void test_redis_namespace_registered(void) {
    script_api    *api = quickjs_script_api();
    script_engine *e   = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);

    bind_redis(e);

    char *type = eval_str(e, "typeof redis");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("object", type);
    free(type);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * test_redis_get_is_function
 * ---------------------------------------------------------------------- */

void test_redis_get_is_function(void) {
    script_api    *api = quickjs_script_api();
    script_engine *e   = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);

    bind_redis(e);

    char *type = eval_str(e, "typeof redis.get");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("function", type);
    free(type);

    type = eval_str(e, "typeof redis.set");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("function", type);
    free(type);

    type = eval_str(e, "typeof redis.command");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("function", type);
    free(type);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * test_redis_get_returns_command_bytes
 *
 * redis.get("foo") should return a RESP bulk-array: *2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
 * We assert the result is a non-empty string starting with '*'.
 * ---------------------------------------------------------------------- */

void test_redis_get_returns_command_bytes(void) {
    script_api    *api = quickjs_script_api();
    script_engine *e   = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);

    bind_redis(e);

    /* Call redis.get("foo") and check the result starts with '*' (RESP array). */
    JSContext *ctx = (JSContext *) qjs_engine_ctx(e);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue redis  = JS_GetPropertyStr(ctx, global, "redis");
    JSValue getfn  = JS_GetPropertyStr(ctx, redis, "get");
    TEST_ASSERT_TRUE(JS_IsFunction(ctx, getfn));

    JSValue arg = JS_NewString(ctx, "foo");
    JSValue ret = JS_Call(ctx, getfn, global, 1, &arg);
    TEST_ASSERT_FALSE(JS_IsException(ret));

    size_t      slen = 0;
    const char *s    = JS_ToCStringLen(ctx, &slen, ret);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_GREATER_THAN_UINT(0, slen);
    TEST_ASSERT_EQUAL_CHAR('*', s[0]);   /* RESP bulk-array marker */

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, getfn);
    JS_FreeValue(ctx, redis);
    JS_FreeValue(ctx, global);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * test_redis_set_returns_command_bytes
 * ---------------------------------------------------------------------- */

void test_redis_set_returns_command_bytes(void) {
    script_api    *api = quickjs_script_api();
    script_engine *e   = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);

    bind_redis(e);

    JSContext *ctx = (JSContext *) qjs_engine_ctx(e);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue redis  = JS_GetPropertyStr(ctx, global, "redis");
    JSValue setfn  = JS_GetPropertyStr(ctx, redis, "set");
    TEST_ASSERT_TRUE(JS_IsFunction(ctx, setfn));

    JSValue args[2] = { JS_NewString(ctx, "mykey"), JS_NewString(ctx, "myval") };
    JSValue ret     = JS_Call(ctx, setfn, global, 2, args);
    TEST_ASSERT_FALSE(JS_IsException(ret));

    size_t      slen = 0;
    const char *s    = JS_ToCStringLen(ctx, &slen, ret);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_GREATER_THAN_UINT(0, slen);
    TEST_ASSERT_EQUAL_CHAR('*', s[0]);

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, setfn);
    JS_FreeValue(ctx, redis);
    JS_FreeValue(ctx, global);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * test_clone_replays_redis_helpers
 *
 * Clone of an engine that had register_helpers called must also expose redis.*
 * ---------------------------------------------------------------------- */

void test_clone_replays_redis_helpers(void) {
    script_api    *api    = quickjs_script_api();
    script_engine *orig   = api->create(NULL);
    TEST_ASSERT_NOT_NULL(orig);

    bind_redis(orig);   /* register on template */

    script_engine *cloned = api->clone(orig);
    TEST_ASSERT_NOT_NULL(cloned);

    /* Clone must also expose redis.get as a function. */
    char *type = eval_str(cloned, "typeof redis");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("object", type);
    free(type);

    type = eval_str(cloned, "typeof redis.get");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("function", type);
    free(type);

    /* And the clone must be able to call it. */
    JSContext *ctx    = (JSContext *) qjs_engine_ctx(cloned);
    JSValue    global = JS_GetGlobalObject(ctx);
    JSValue    redis  = JS_GetPropertyStr(ctx, global, "redis");
    JSValue    getfn  = JS_GetPropertyStr(ctx, redis, "get");
    JSValue    arg    = JS_NewString(ctx, "clonekey");
    JSValue    ret    = JS_Call(ctx, getfn, global, 1, &arg);

    TEST_ASSERT_FALSE(JS_IsException(ret));
    size_t      slen = 0;
    const char *s    = JS_ToCStringLen(ctx, &slen, ret);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_GREATER_THAN_UINT(0, slen);

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, getfn);
    JS_FreeValue(ctx, redis);
    JS_FreeValue(ctx, global);

    api->destroy(orig);
    api->destroy(cloned);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_redis_namespace_registered);
    RUN_TEST(test_redis_get_is_function);
    RUN_TEST(test_redis_get_returns_command_bytes);
    RUN_TEST(test_redis_set_returns_command_bytes);
    RUN_TEST(test_clone_replays_redis_helpers);

    return UNITY_END();
}

#endif /* WRKX_HAVE_QUICKJS */
