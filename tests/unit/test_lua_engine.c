/* tests/unit/test_lua_engine.c
 *
 * Unit tests for the Request Layer LuaJIT engine (ADR 0001 P1-4):
 *   - src/scripting/lua/engine.c   — script_api vtable + helper registration
 *   - src/scripting/session.c      — per-connection KV store
 *   - src/scripting/lua/http1_helpers.c — glue module exemplar
 *
 * Mirrors tests/unit/test_script.c: loads small Lua scripts through the frozen
 * script_api and asserts the init/request/response/done hooks behave as the
 * legacy src/script.c did.
 *
 * Note on LuaJIT + ASAN: like test_script, this binary links LuaJIT and is
 * intentionally excluded from make test-asan (custom mmap allocator conflicts
 * with ASAN shadow memory — see t16).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "scripting/script_api.h"
#include "scripting/lua/engine.h"
#include "scripting/helper_tag.h"
#include "orchestrator.h"

void setUp(void) {}
void tearDown(void) {}

/* Write a temp Lua script, return its path (caller does not free; static). */
static const char *write_script(const char *body) {
    static char path[] = "/tmp/wrkx_lua_engine_XXXXXX";
    /* Reset template each call (mkstemp mutates it). */
    strcpy(path, "/tmp/wrkx_lua_engine_XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    FILE *f = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(body, f);
    fclose(f);
    return path;
}

/* -------------------------------------------------------------------------
 * create / destroy
 * ---------------------------------------------------------------------- */

void test_create_no_file_succeeds(void) {
    script_api *api = lua_script_api();
    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_EQUAL_STRING("lua", api->name);

    script_engine *e = api->create(NULL);
    TEST_ASSERT_NOT_NULL(e);
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * request hook
 * ---------------------------------------------------------------------- */

void test_request_default_is_get(void) {
    script_api *api = lua_script_api();
    script_engine *e = api->create(NULL);
    api->init(e, 0, 1);

    size_t len = 0;
    char *buf = api->request(e, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(len > 0);
    /* default request is a GET / HTTP/1.1 line (from wrk.lua). */
    TEST_ASSERT_NOT_NULL(strstr(buf, "GET"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "HTTP/1.1"));

    free(buf);
    api->destroy(e);
}

void test_request_custom_function(void) {
    const char *path = write_script("function request() return 'PING' end\n");
    script_api *api = lua_script_api();
    script_engine *e = api->create(path);
    api->init(e, 0, 1);

    size_t len = 0;
    char *buf = api->request(e, &len);
    TEST_ASSERT_EQUAL_UINT(4, len);
    TEST_ASSERT_EQUAL_MEMORY("PING", buf, 4);

    free(buf);
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * response hook
 * ---------------------------------------------------------------------- */

void test_response_hook_invoked_with_status(void) {
    const char *path = write_script(
        "seen_status = nil\n"
        "function response(status, headers, body) seen_status = status end\n");
    script_api *api = lua_script_api();
    script_engine *e = api->create(path);
    api->init(e, 0, 1);

    api->response(e, 200, 123, 4567);

    lua_State *L = (lua_State *) lua_engine_state(e);
    lua_getglobal(L, "seen_status");
    TEST_ASSERT_TRUE(lua_isnumber(L, -1));
    TEST_ASSERT_EQUAL_INT(200, (int) lua_tointeger(L, -1));
    lua_pop(L, 1);

    api->destroy(e);
}

void test_response_hook_absent_is_noop(void) {
    /* No response() defined — must not crash. */
    script_api *api = lua_script_api();
    script_engine *e = api->create(NULL);
    api->init(e, 0, 1);
    api->response(e, 404, 0, 0);
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * done hook
 * ---------------------------------------------------------------------- */

void test_done_hook_receives_summary(void) {
    const char *path = write_script(
        "done_requests = nil\n"
        "function done(summary, latency, requests)\n"
        "  done_requests = summary.requests\n"
        "end\n");
    script_api *api = lua_script_api();
    script_engine *e = api->create(path);
    api->init(e, 0, 1);

    orchestrator_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.requests   = 42;
    stats.elapsed_us = 1000000;
    api->done(e, &stats);

    lua_State *L = (lua_State *) lua_engine_state(e);
    lua_getglobal(L, "done_requests");
    TEST_ASSERT_TRUE(lua_isnumber(L, -1));
    TEST_ASSERT_EQUAL_INT(42, (int) lua_tointeger(L, -1));
    lua_pop(L, 1);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * configure slot (ADR 0002 Decision 3)
 * ---------------------------------------------------------------------- */

void test_configure_sets_url_fields(void) {
    script_api *api = lua_script_api();
    TEST_ASSERT_NOT_NULL(api->configure);

    script_engine *e = api->create(NULL);
    int rc = api->configure(e, "http://bench.example.com:9090/api", NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);

    lua_State *L = (lua_State *) lua_engine_state(e);
    lua_getglobal(L, "wrk");

    lua_getfield(L, -1, "scheme");
    TEST_ASSERT_EQUAL_STRING("http", lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, -1, "host");
    TEST_ASSERT_EQUAL_STRING("bench.example.com", lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, -1, "port");
    TEST_ASSERT_EQUAL_INT(9090, (int) lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, -1, "path");
    TEST_ASSERT_EQUAL_STRING("/api", lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_pop(L, 1); /* wrk */
    api->destroy(e);
}

void test_configure_installs_headers(void) {
    script_api *api = lua_script_api();
    script_engine *e = api->create(NULL);

    const char *hdrs[] = {"X-Foo: bar", "X-Req-Id: 42"};
    int rc = api->configure(e, "http://localhost/", hdrs, 2);
    TEST_ASSERT_EQUAL_INT(0, rc);

    lua_State *L = (lua_State *) lua_engine_state(e);

    TEST_ASSERT_EQUAL_INT(0, luaL_dostring(L, "hdr_foo   = wrk.headers['X-Foo']"));
    TEST_ASSERT_EQUAL_INT(0, luaL_dostring(L, "hdr_reqid = wrk.headers['X-Req-Id']"));

    lua_getglobal(L, "hdr_foo");
    TEST_ASSERT_EQUAL_STRING("bar", lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_getglobal(L, "hdr_reqid");
    TEST_ASSERT_EQUAL_STRING("42", lua_tostring(L, -1));
    lua_pop(L, 1);

    api->destroy(e);
}

void test_configure_null_url_is_noop(void) {
    script_api *api = lua_script_api();
    script_engine *e = api->create(NULL);
    /* NULL url and zero headers must not crash and must return 0. */
    TEST_ASSERT_EQUAL_INT(0, api->configure(e, NULL, NULL, 0));
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * helper registration + glue module (http.name)
 * ---------------------------------------------------------------------- */

void test_http_helper_registered_during_init(void) {
    script_api *api = lua_script_api();
    script_engine *e = api->create(NULL);
    api->init(e, 0, 1);

    lua_State *L = (lua_State *) lua_engine_state(e);
    /* http.name() comes from the glue module, calling http1_protocol()->name */
    int rc = luaL_dostring(L, "glue_result = http.name()");
    TEST_ASSERT_EQUAL_INT(0, rc);
    lua_getglobal(L, "glue_result");
    TEST_ASSERT_EQUAL_STRING("http/1.1", lua_tostring(L, -1));
    lua_pop(L, 1);

    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * capability detection (ADR 0005, Phase 5, t069)
 * ---------------------------------------------------------------------- */

void test_capabilities_null_script_is_static(void) {
    script_api *api = lua_script_api();
    TEST_ASSERT_NOT_NULL(api->capabilities);

    script_engine *e = api->create(NULL);
    TEST_ASSERT_EQUAL_UINT32(0, api->capabilities(e));
    api->destroy(e);
}

void test_capabilities_static_request_is_static(void) {
    /* A script with no global request/response is a static default workload. */
    const char *path = write_script("local x = 1\n");
    script_api *api = lua_script_api();
    script_engine *e = api->create(path);
    TEST_ASSERT_EQUAL_UINT32(0, api->capabilities(e));
    api->destroy(e);
}

void test_capabilities_custom_request_is_dynamic(void) {
    const char *path =
        write_script("function request() return wrk.format('GET','/'..1) end\n");
    script_api *api = lua_script_api();
    script_engine *e = api->create(path);

    uint32_t caps = api->capabilities(e);
    TEST_ASSERT_TRUE(caps & SCRIPT_CAP_DYNAMIC_REQUEST);
    TEST_ASSERT_FALSE(caps & SCRIPT_CAP_RESPONSE_HOOK);
    api->destroy(e);
}

void test_capabilities_response_hook_detected(void) {
    const char *path =
        write_script("function response(status, headers, body) end\n");
    script_api *api = lua_script_api();
    script_engine *e = api->create(path);

    uint32_t caps = api->capabilities(e);
    TEST_ASSERT_TRUE(caps & SCRIPT_CAP_RESPONSE_HOOK);
    TEST_ASSERT_FALSE(caps & SCRIPT_CAP_DYNAMIC_REQUEST);
    api->destroy(e);
}

/* -------------------------------------------------------------------------
 * vtable helper registration + engine-tagged namespace selection (t069)
 * ---------------------------------------------------------------------- */

static int sample_helper(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    lua_pushstring(L, "from_helper");
    return 1;
}

void test_register_helpers_via_vtable(void) {
    script_api *api = lua_script_api();
    TEST_ASSERT_NOT_NULL(api->register_helpers);

    script_engine *e = api->create(NULL);
    const script_helper helpers[] = { { "ping", sample_helper } };
    api->register_helpers(e, "foo", helpers, 1);

    lua_State *L = (lua_State *) lua_engine_state(e);
    TEST_ASSERT_EQUAL_INT(0, luaL_dostring(L, "r = foo.ping()"));
    lua_getglobal(L, "r");
    TEST_ASSERT_EQUAL_STRING("from_helper", lua_tostring(L, -1));
    lua_pop(L, 1);

    api->destroy(e);
}

void test_engine_tagged_namespace_selection(void) {
    char bare[64];

    /* untagged "a" → bound for every engine, unchanged */
    TEST_ASSERT_TRUE(helper_ns_select("a", "lua", bare, sizeof bare));
    TEST_ASSERT_EQUAL_STRING("a", bare);

    /* "b@lua" with active engine "lua" → bound, suffix stripped */
    TEST_ASSERT_TRUE(helper_ns_select("b@lua", "lua", bare, sizeof bare));
    TEST_ASSERT_EQUAL_STRING("b", bare);

    /* "c@quickjs" with active engine "lua" → skipped */
    TEST_ASSERT_FALSE(helper_ns_select("c@quickjs", "lua", bare, sizeof bare));
}

/* -------------------------------------------------------------------------
 * session store
 * ---------------------------------------------------------------------- */

void test_session_set_get(void) {
    session *s = session_create();
    TEST_ASSERT_NOT_NULL(s);

    TEST_ASSERT_NULL(session_get(s, "missing"));
    session_set(s, "token", "abc123");
    TEST_ASSERT_EQUAL_STRING("abc123", session_get(s, "token"));

    /* overwrite */
    session_set(s, "token", "xyz789");
    TEST_ASSERT_EQUAL_STRING("xyz789", session_get(s, "token"));

    /* second key */
    session_set(s, "user", "alice");
    TEST_ASSERT_EQUAL_STRING("alice", session_get(s, "user"));
    TEST_ASSERT_EQUAL_STRING("xyz789", session_get(s, "token"));

    session_destroy(s);
}

void test_session_null_safe(void) {
    TEST_ASSERT_NULL(session_get(NULL, "k"));
    session_set(NULL, "k", "v");   /* must not crash */
    session_destroy(NULL);          /* must not crash */
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_no_file_succeeds);

    RUN_TEST(test_request_default_is_get);
    RUN_TEST(test_request_custom_function);

    RUN_TEST(test_response_hook_invoked_with_status);
    RUN_TEST(test_response_hook_absent_is_noop);

    RUN_TEST(test_done_hook_receives_summary);

    RUN_TEST(test_configure_sets_url_fields);
    RUN_TEST(test_configure_installs_headers);
    RUN_TEST(test_configure_null_url_is_noop);

    RUN_TEST(test_http_helper_registered_during_init);

    RUN_TEST(test_capabilities_null_script_is_static);
    RUN_TEST(test_capabilities_static_request_is_static);
    RUN_TEST(test_capabilities_custom_request_is_dynamic);
    RUN_TEST(test_capabilities_response_hook_detected);

    RUN_TEST(test_register_helpers_via_vtable);
    RUN_TEST(test_engine_tagged_namespace_selection);

    RUN_TEST(test_session_set_get);
    RUN_TEST(test_session_null_safe);

    return UNITY_END();
}
