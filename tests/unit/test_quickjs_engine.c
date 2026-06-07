/* tests/unit/test_quickjs_engine.c
 *
 * Unit tests for the QuickJS Request-Layer engine lifecycle (ADR 0005, t072):
 *   create / configure / init / destroy
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

void setUp(void) {}
void tearDown(void) {}

/* Write a temp JS script, return its path (static buffer; valid until next call). */
static const char *write_script(const char *body) {
    static char path[] = "/tmp/wrkx_qjs_engine_XXXXXX";
    strcpy(path, "/tmp/wrkx_qjs_engine_XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    FILE *f = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(body, f);
    fclose(f);
    return path;
}

/* Evaluate a JS expression inside the engine, return a heap copy of the
 * string result (caller frees), or NULL on error / non-string. */
static char *engine_eval_str(script_engine *e, const char *js) {
    JSContext *ctx = (JSContext *) qjs_engine_ctx(e);
    JSValue v = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { JS_FreeValue(ctx, v); return NULL; }
    const char *cs = JS_ToCString(ctx, v);
    char *out = cs ? strdup(cs) : NULL;
    JS_FreeCString(ctx, cs);
    JS_FreeValue(ctx, v);
    return out;
}

/* -------------------------------------------------------------------------
 * create / destroy
 * ---------------------------------------------------------------------- */

void test_create_empty_script_succeeds(void) {
    const char *path = write_script("");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NOT_NULL(e);
    api->destroy(e);
}

void test_create_syntax_error_returns_null(void) {
    const char *path = write_script("function (");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NULL(e);   /* parse error → NULL, no crash */
}

void test_create_null_file_succeeds(void) {
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * configure
 * ---------------------------------------------------------------------- */

void test_configure_exposes_wrk_globals(void) {
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);

    int rc = api->configure(e, "http://example.com:9090/bench", NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);

    char *scheme = engine_eval_str(e, "wrk.scheme");
    char *host   = engine_eval_str(e, "wrk.host");
    char *port   = engine_eval_str(e, "String(wrk.port)");
    char *path   = engine_eval_str(e, "wrk.path");

    TEST_ASSERT_NOT_NULL(scheme); TEST_ASSERT_EQUAL_STRING("http",        scheme);
    TEST_ASSERT_NOT_NULL(host);   TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_NOT_NULL(port);   TEST_ASSERT_EQUAL_STRING("9090",        port);
    TEST_ASSERT_NOT_NULL(path);   TEST_ASSERT_EQUAL_STRING("/bench",      path);

    free(scheme); free(host); free(port); free(path);
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * init
 * ---------------------------------------------------------------------- */

void test_init_calls_js_function(void) {
    const char *path = write_script(
        "var x = 0;\n"
        "function init(tid, conns) { x = 1; }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NOT_NULL(e);

    api->init(e, 0, 1);

    char *val = engine_eval_str(e, "String(x)");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("1", val);
    free(val);

    api->destroy(e);
}

void test_init_missing_function_is_safe(void) {
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);
    api->init(e, 0, 1);   /* no init() export → must not crash */
    api->destroy(e);
}

void test_init_exception_is_safe(void) {
    const char *path = write_script(
        "function init() { throw new Error('boom'); }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NOT_NULL(e);
    api->init(e, 0, 1);   /* init throws → must not crash or abort */
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * request hook (t073)
 * ---------------------------------------------------------------------- */

void test_request_returns_exact_bytes(void) {
    const char *path = write_script(
        "function request() { return 'GET / HTTP/1.1\\r\\n\\r\\n'; }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NOT_NULL(e);

    size_t len = 0;
    char *buf = api->request(e, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT(strlen("GET / HTTP/1.1\r\n\r\n"), len);
    TEST_ASSERT_EQUAL_MEMORY("GET / HTTP/1.1\r\n\r\n", buf, len);

    free(buf);
    api->destroy(e);
}

void test_request_state_persists_across_calls(void) {
    const char *path = write_script(
        "var n = 0;\n"
        "function request() { n++; return String(n); }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NOT_NULL(e);

    size_t len = 0;
    char *r1 = api->request(e, &len); TEST_ASSERT_EQUAL_STRING("1", r1); free(r1);
    char *r2 = api->request(e, &len); TEST_ASSERT_EQUAL_STRING("2", r2); free(r2);
    char *r3 = api->request(e, &len); TEST_ASSERT_EQUAL_STRING("3", r3); free(r3);

    api->destroy(e);
}

void test_request_exception_returns_null(void) {
    const char *path = write_script(
        "function request() { throw new Error('bad'); }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_NOT_NULL(e);

    size_t len = 99;
    char *buf = api->request(e, &len);
    TEST_ASSERT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT(0, len);

    api->destroy(e);
}

void test_request_missing_returns_null(void) {
    /* No request() defined — must return NULL without crash. */
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);

    size_t len = 99;
    char *buf = api->request(e, &len);
    TEST_ASSERT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT(0, len);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * capabilities (t073)
 * ---------------------------------------------------------------------- */

void test_capabilities_empty_script_is_zero(void) {
    script_api *api = quickjs_script_api();
    TEST_ASSERT_NOT_NULL(api->capabilities);
    script_engine *e = api->create(NULL);
    TEST_ASSERT_EQUAL_UINT32(0, api->capabilities(e));
    api->destroy(e);
}

void test_capabilities_request_only_is_dynamic(void) {
    const char *path = write_script("function request() { return 'x'; }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);

    uint32_t caps = api->capabilities(e);
    TEST_ASSERT_TRUE(caps & SCRIPT_CAP_DYNAMIC_REQUEST);
    TEST_ASSERT_FALSE(caps & SCRIPT_CAP_RESPONSE_HOOK);

    api->destroy(e);
}

void test_capabilities_with_response_sets_both_bits(void) {
    const char *path = write_script(
        "function request()  { return 'x'; }\n"
        "function response() { }\n");
    script_api *api = quickjs_script_api();
    script_engine *e = api->create(path);

    uint32_t caps = api->capabilities(e);
    TEST_ASSERT_TRUE(caps & SCRIPT_CAP_DYNAMIC_REQUEST);
    TEST_ASSERT_TRUE(caps & SCRIPT_CAP_RESPONSE_HOOK);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_empty_script_succeeds);
    RUN_TEST(test_create_syntax_error_returns_null);
    RUN_TEST(test_create_null_file_succeeds);

    RUN_TEST(test_configure_exposes_wrk_globals);

    RUN_TEST(test_init_calls_js_function);
    RUN_TEST(test_init_missing_function_is_safe);
    RUN_TEST(test_init_exception_is_safe);

    RUN_TEST(test_request_returns_exact_bytes);
    RUN_TEST(test_request_state_persists_across_calls);
    RUN_TEST(test_request_exception_returns_null);
    RUN_TEST(test_request_missing_returns_null);

    RUN_TEST(test_capabilities_empty_script_is_zero);
    RUN_TEST(test_capabilities_request_only_is_dynamic);
    RUN_TEST(test_capabilities_with_response_sets_both_bits);

    return UNITY_END();
}


#endif /* WRKX_HAVE_QUICKJS */
