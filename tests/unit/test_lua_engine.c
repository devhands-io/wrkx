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

    RUN_TEST(test_http_helper_registered_during_init);

    RUN_TEST(test_session_set_get);
    RUN_TEST(test_session_null_safe);

    return UNITY_END();
}
