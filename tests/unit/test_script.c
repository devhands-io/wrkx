/* tests/unit/test_script.c
 *
 * Unit tests for src/script.c — the Lua embedding layer.
 *
 * Note on architecture: script.c has two classes of functions:
 *   - Non-thread-dependent: testable here (script_create, script_request,
 *     script_is_static, script_want_response, script_has_done,
 *     script_parse_url, buffer_append, buffer_reset)
 *   - Thread-dependent: require a live aeEventLoop + thread struct;
 *     covered by the E2E Lua hook tests (lua_hooks.sh etc.)
 *
 * Note on LuaJIT + ASAN: LuaJIT uses custom mmap-based memory management
 * that conflicts with ASAN. This binary is intentionally excluded from
 * make test-asan. See t16 for rationale.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "script.h"
#include "wrk.h"
#include "http_parser.h"

void setUp(void) {}
void tearDown(void) {}

static char *no_headers[] = {NULL};

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Call wrk.init({}) to build the default request after script_create.
 * In a real run this happens inside script_init(); here we call it
 * directly so script_request() has a wrk.request closure to invoke. */
static void init_wrk(lua_State *L) {
    if (luaL_dostring(L, "wrk.init({})") != 0) {
        fprintf(stderr, "wrk.init failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

/* -------------------------------------------------------------------------
 * script_create — URL field injection
 * ---------------------------------------------------------------------- */

void test_script_create_no_file_succeeds(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    lua_close(L);
}

void test_script_create_url_scheme(void) {
    lua_State *L = script_create(NULL, "http://example.com:9090/api", no_headers);
    TEST_ASSERT_NOT_NULL(L);

    lua_getglobal(L, "wrk");
    lua_getfield(L, -1, "scheme");
    TEST_ASSERT_EQUAL_STRING("http", lua_tostring(L, -1));
    lua_pop(L, 2);
    lua_close(L);
}

void test_script_create_url_host(void) {
    lua_State *L = script_create(NULL, "http://example.com:9090/api", no_headers);
    TEST_ASSERT_NOT_NULL(L);

    lua_getglobal(L, "wrk");
    lua_getfield(L, -1, "host");
    TEST_ASSERT_EQUAL_STRING("example.com", lua_tostring(L, -1));
    lua_pop(L, 2);
    lua_close(L);
}

void test_script_create_url_port(void) {
    lua_State *L = script_create(NULL, "http://example.com:9090/api", no_headers);
    TEST_ASSERT_NOT_NULL(L);

    lua_getglobal(L, "wrk");
    lua_getfield(L, -1, "port");
    TEST_ASSERT_EQUAL_STRING("9090", lua_tostring(L, -1));
    lua_pop(L, 2);
    lua_close(L);
}

void test_script_create_url_path(void) {
    lua_State *L = script_create(NULL, "http://example.com:9090/api", no_headers);
    TEST_ASSERT_NOT_NULL(L);

    lua_getglobal(L, "wrk");
    lua_getfield(L, -1, "path");
    TEST_ASSERT_EQUAL_STRING("/api", lua_tostring(L, -1));
    lua_pop(L, 2);
    lua_close(L);
}

/* -------------------------------------------------------------------------
 * script_request
 * ---------------------------------------------------------------------- */

void test_script_request_default_is_get(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    init_wrk(L);

    char  *buf = NULL;
    size_t len = 0;
    script_request(L, &buf, &len);

    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "GET"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "HTTP/1.1"));

    free(buf);
    lua_close(L);
}

void test_script_request_custom_function(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);

    /* Custom global request() overrides wrk.request */
    luaL_dostring(L, "function request() return 'PING' end");

    char  *buf = NULL;
    size_t len = 0;
    script_request(L, &buf, &len);

    TEST_ASSERT_EQUAL_UINT(4, len);
    TEST_ASSERT_EQUAL_MEMORY("PING", buf, 4);

    free(buf);
    lua_close(L);
}

/* -------------------------------------------------------------------------
 * script_is_static
 * ---------------------------------------------------------------------- */

void test_script_is_static_without_request_fn(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    /* No global request() → static */
    TEST_ASSERT_TRUE(script_is_static(L));
    lua_close(L);
}

void test_script_is_static_with_request_fn(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    luaL_dostring(L, "function request() return wrk.format() end");
    TEST_ASSERT_FALSE(script_is_static(L));
    lua_close(L);
}

/* -------------------------------------------------------------------------
 * script_want_response
 * ---------------------------------------------------------------------- */

void test_script_want_response_false_by_default(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    TEST_ASSERT_FALSE(script_want_response(L));
    lua_close(L);
}

void test_script_want_response_true_when_defined(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    luaL_dostring(L, "function response(status, headers, body) end");
    TEST_ASSERT_TRUE(script_want_response(L));
    lua_close(L);
}

/* -------------------------------------------------------------------------
 * script_has_done
 * ---------------------------------------------------------------------- */

void test_script_has_done_false_by_default(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    TEST_ASSERT_FALSE(script_has_done(L));
    lua_close(L);
}

void test_script_has_done_true_when_defined(void) {
    lua_State *L = script_create(NULL, "http://localhost/", no_headers);
    TEST_ASSERT_NOT_NULL(L);
    luaL_dostring(L, "function done(summary, latency, requests) end");
    TEST_ASSERT_TRUE(script_has_done(L));
    lua_close(L);
}

/* -------------------------------------------------------------------------
 * script_parse_url
 * ---------------------------------------------------------------------- */

void test_script_parse_url_sets_field_bits(void) {
    struct http_parser_url parts;
    memset(&parts, 0, sizeof(parts));

    /* script_parse_url returns 1 for a valid URL (has schema + host),
     * 0 for invalid — this is inverted from the raw http_parser convention. */
    int rc = script_parse_url("http://host.com:8080/foo", &parts);
    TEST_ASSERT_EQUAL_INT(1, rc);

    TEST_ASSERT_TRUE(parts.field_set & (1 << UF_SCHEMA));
    TEST_ASSERT_TRUE(parts.field_set & (1 << UF_HOST));
    TEST_ASSERT_TRUE(parts.field_set & (1 << UF_PORT));
    TEST_ASSERT_TRUE(parts.field_set & (1 << UF_PATH));
}

/* -------------------------------------------------------------------------
 * buffer_append / buffer_reset
 * ---------------------------------------------------------------------- */

void test_buffer_append_stores_data(void) {
    buffer b = {0};
    buffer_append(&b, "hello", 5);

    TEST_ASSERT_NOT_NULL(b.buffer);
    TEST_ASSERT_EQUAL_INT(5, b.cursor - b.buffer);
    TEST_ASSERT_EQUAL_MEMORY("hello", b.buffer, 5);

    free(b.buffer);
}

void test_buffer_reset_rewinds_cursor(void) {
    buffer b = {0};
    buffer_append(&b, "hello", 5);
    buffer_reset(&b);

    /* cursor must point back to start of buffer */
    TEST_ASSERT_EQUAL_PTR(b.buffer, b.cursor);

    free(b.buffer);
}

void test_buffer_append_multiple_chunks(void) {
    buffer b = {0};
    buffer_append(&b, "foo", 3);
    buffer_append(&b, "bar", 3);

    TEST_ASSERT_EQUAL_INT(6, b.cursor - b.buffer);
    TEST_ASSERT_EQUAL_MEMORY("foobar", b.buffer, 6);

    free(b.buffer);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    /* script_create */
    RUN_TEST(test_script_create_no_file_succeeds);
    RUN_TEST(test_script_create_url_scheme);
    RUN_TEST(test_script_create_url_host);
    RUN_TEST(test_script_create_url_port);
    RUN_TEST(test_script_create_url_path);

    /* script_request */
    RUN_TEST(test_script_request_default_is_get);
    RUN_TEST(test_script_request_custom_function);

    /* script_is_static */
    RUN_TEST(test_script_is_static_without_request_fn);
    RUN_TEST(test_script_is_static_with_request_fn);

    /* script_want_response */
    RUN_TEST(test_script_want_response_false_by_default);
    RUN_TEST(test_script_want_response_true_when_defined);

    /* script_has_done */
    RUN_TEST(test_script_has_done_false_by_default);
    RUN_TEST(test_script_has_done_true_when_defined);

    /* script_parse_url */
    RUN_TEST(test_script_parse_url_sets_field_bits);

    /* buffer */
    RUN_TEST(test_buffer_append_stores_data);
    RUN_TEST(test_buffer_reset_rewinds_cursor);
    RUN_TEST(test_buffer_append_multiple_chunks);

    return UNITY_END();
}
