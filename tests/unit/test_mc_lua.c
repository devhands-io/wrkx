/*
 * tests/unit/test_mc_lua.c
 *
 * Unit tests for the memcached Lua helper namespace (ADR 0005, P4-1, t061).
 *
 * Tests the "memcached" Lua global registered by mc_lua_helpers.c:
 *   - get / set / delete / incr / decr return correctly encoded bytes
 *   - set options table (flags, exptime) applied correctly
 *   - incr / decr default delta of 1 when omitted
 *   - all error cases raise Lua errors (caught by pcall), not crashes
 *
 * Intentionally excluded from test-asan (LuaJIT allocator conflicts).
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
#include "mc_lua_helpers.h"   /* from extensions/memcached/ */

/* -------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------- */

static script_api    *api;
static script_engine *engine;
static lua_State     *L;

void setUp(void) {
    api    = lua_script_api();
    engine = api->create(NULL);
    TEST_ASSERT_NOT_NULL(engine);

    api->init(engine, 0, 1);

    script_register_helpers(engine, "memcached",
                            mc_lua_helpers, mc_lua_helpers_count);

    L = (lua_State *)lua_engine_state(engine);
    TEST_ASSERT_NOT_NULL(L);
}

void tearDown(void) {
    api->destroy(engine);
    engine = NULL;
    L      = NULL;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Call memcached.<fn>(args...) via pcall.
 * Push args first, then call this; it retrieves the function and moves it
 * below the already-pushed args before calling lua_pcall.
 * Returns 0 on success with result on stack, non-zero on Lua error. */
static int call_helper(const char *fn, int nargs) {
    lua_getglobal(L, "memcached");
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    /* fn is now on top; args sit below it — move fn just under the args */
    if (nargs > 0)
        lua_insert(L, -(nargs + 1));
    return lua_pcall(L, nargs, 1, 0);
}

/* -------------------------------------------------------------------------
 * Namespace registration
 * ---------------------------------------------------------------------- */

void test_namespace_exists(void) {
    lua_getglobal(L, "memcached");
    TEST_ASSERT_TRUE(lua_istable(L, -1));
    lua_pop(L, 1);
}

void test_get_is_callable(void) {
    lua_getglobal(L, "memcached");
    lua_getfield(L, -1, "get");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

void test_set_is_callable(void) {
    lua_getglobal(L, "memcached");
    lua_getfield(L, -1, "set");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

void test_delete_is_callable(void) {
    lua_getglobal(L, "memcached");
    lua_getfield(L, -1, "delete");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

void test_incr_is_callable(void) {
    lua_getglobal(L, "memcached");
    lua_getfield(L, -1, "incr");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

void test_decr_is_callable(void) {
    lua_getglobal(L, "memcached");
    lua_getfield(L, -1, "decr");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

/* -------------------------------------------------------------------------
 * memcached.get
 * ---------------------------------------------------------------------- */

void test_get_encodes_correctly(void) {
    lua_pushstring(L, "mykey");
    int rc = call_helper("get", 1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "get mykey\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_get_no_args_raises_error(void) {
    int rc = call_helper("get", 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "key argument required"));
    lua_pop(L, 1);
}

void test_get_non_string_key_raises_error(void) {
    lua_pushnumber(L, 42);
    int rc = call_helper("get", 1);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "must be a string"));
    lua_pop(L, 1);
}

void test_get_key_with_space_raises_error(void) {
    lua_pushstring(L, "bad key");
    int rc = call_helper("get", 1);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * memcached.set
 * ---------------------------------------------------------------------- */

void test_set_encodes_correctly(void) {
    lua_pushstring(L, "k");
    lua_pushstring(L, "hello");
    int rc = call_helper("set", 2);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "set k 0 0 5\r\nhello\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_set_with_flags_and_exptime(void) {
    lua_pushstring(L, "key");
    lua_pushstring(L, "val");
    lua_newtable(L);
    lua_pushnumber(L, 7);   lua_setfield(L, -2, "flags");
    lua_pushnumber(L, 300); lua_setfield(L, -2, "exptime");
    int rc = call_helper("set", 3);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "set key 7 300 3\r\nval\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_set_flags_only(void) {
    lua_pushstring(L, "k");
    lua_pushstring(L, "v");
    lua_newtable(L);
    lua_pushnumber(L, 15); lua_setfield(L, -2, "flags");
    int rc = call_helper("set", 3);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "set k 15 0 1\r\nv\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_set_no_args_raises_error(void) {
    int rc = call_helper("set", 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "key and value required"));
    lua_pop(L, 1);
}

void test_set_missing_value_raises_error(void) {
    lua_pushstring(L, "k");
    int rc = call_helper("set", 1);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    lua_pop(L, 1);
}

void test_set_non_string_key_raises_error(void) {
    lua_pushnumber(L, 1);
    lua_pushstring(L, "v");
    int rc = call_helper("set", 2);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "key must be a string"));
    lua_pop(L, 1);
}

void test_set_non_string_value_raises_error(void) {
    lua_pushstring(L, "k");
    lua_pushnumber(L, 42);
    int rc = call_helper("set", 2);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "value must be a string"));
    lua_pop(L, 1);
}

void test_set_non_table_opts_raises_error(void) {
    lua_pushstring(L, "k");
    lua_pushstring(L, "v");
    lua_pushstring(L, "not-a-table");
    int rc = call_helper("set", 3);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "opts must be a table"));
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * memcached.delete
 * ---------------------------------------------------------------------- */

void test_delete_encodes_correctly(void) {
    lua_pushstring(L, "mykey");
    int rc = call_helper("delete", 1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "delete mykey\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_delete_no_args_raises_error(void) {
    int rc = call_helper("delete", 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "key argument required"));
    lua_pop(L, 1);
}

void test_delete_non_string_key_raises_error(void) {
    lua_pushnumber(L, 99);
    int rc = call_helper("delete", 1);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * memcached.incr
 * ---------------------------------------------------------------------- */

void test_incr_with_delta(void) {
    lua_pushstring(L, "counter");
    lua_pushnumber(L, 5);
    int rc = call_helper("incr", 2);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "incr counter 5\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_incr_default_delta(void) {
    lua_pushstring(L, "hits");
    int rc = call_helper("incr", 1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "incr hits 1\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_incr_no_args_raises_error(void) {
    int rc = call_helper("incr", 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "key argument required"));
    lua_pop(L, 1);
}

void test_incr_non_string_key_raises_error(void) {
    lua_pushnumber(L, 1);
    int rc = call_helper("incr", 1);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    lua_pop(L, 1);
}

void test_incr_non_number_delta_raises_error(void) {
    lua_pushstring(L, "k");
    lua_pushstring(L, "not-a-number");
    int rc = call_helper("incr", 2);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "delta must be a number"));
    lua_pop(L, 1);
}

void test_incr_negative_delta_raises_error(void) {
    lua_pushstring(L, "k");
    lua_pushnumber(L, -1);
    int rc = call_helper("incr", 2);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "must not be negative"));
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * memcached.decr
 * ---------------------------------------------------------------------- */

void test_decr_with_delta(void) {
    lua_pushstring(L, "counter");
    lua_pushnumber(L, 10);
    int rc = call_helper("decr", 2);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "decr counter 10\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_decr_default_delta(void) {
    lua_pushstring(L, "hits");
    int rc = call_helper("decr", 1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "decr hits 1\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_decr_no_args_raises_error(void) {
    int rc = call_helper("decr", 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "key argument required"));
    lua_pop(L, 1);
}

void test_decr_negative_delta_raises_error(void) {
    lua_pushstring(L, "k");
    lua_pushnumber(L, -5);
    int rc = call_helper("decr", 2);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(strstr(err, "must not be negative"));
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * Integration: helpers usable as request() return value
 * ---------------------------------------------------------------------- */

void test_get_usable_as_request(void) {
    const char *script =
        "function request()\n"
        "    return memcached.get('bench:key')\n"
        "end\n";
    int rc = luaL_dostring(L, script);
    if (rc != 0) TEST_FAIL_MESSAGE(lua_tostring(L, -1));

    size_t len = 0;
    char *buf = api->request(engine, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(len > 0);
    const char *expected = "get bench:key\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, len);
    free(buf);
}

void test_set_usable_as_request(void) {
    const char *script =
        "function request()\n"
        "    return memcached.set('k', 'hello')\n"
        "end\n";
    int rc = luaL_dostring(L, script);
    if (rc != 0) TEST_FAIL_MESSAGE(lua_tostring(L, -1));

    size_t len = 0;
    char *buf = api->request(engine, &len);
    TEST_ASSERT_NOT_NULL(buf);
    const char *expected = "set k 0 0 5\r\nhello\r\n";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, len);
    free(buf);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    /* Namespace */
    RUN_TEST(test_namespace_exists);
    RUN_TEST(test_get_is_callable);
    RUN_TEST(test_set_is_callable);
    RUN_TEST(test_delete_is_callable);
    RUN_TEST(test_incr_is_callable);
    RUN_TEST(test_decr_is_callable);

    /* get */
    RUN_TEST(test_get_encodes_correctly);
    RUN_TEST(test_get_no_args_raises_error);
    RUN_TEST(test_get_non_string_key_raises_error);
    RUN_TEST(test_get_key_with_space_raises_error);

    /* set */
    RUN_TEST(test_set_encodes_correctly);
    RUN_TEST(test_set_with_flags_and_exptime);
    RUN_TEST(test_set_flags_only);
    RUN_TEST(test_set_no_args_raises_error);
    RUN_TEST(test_set_missing_value_raises_error);
    RUN_TEST(test_set_non_string_key_raises_error);
    RUN_TEST(test_set_non_string_value_raises_error);
    RUN_TEST(test_set_non_table_opts_raises_error);

    /* delete */
    RUN_TEST(test_delete_encodes_correctly);
    RUN_TEST(test_delete_no_args_raises_error);
    RUN_TEST(test_delete_non_string_key_raises_error);

    /* incr */
    RUN_TEST(test_incr_with_delta);
    RUN_TEST(test_incr_default_delta);
    RUN_TEST(test_incr_no_args_raises_error);
    RUN_TEST(test_incr_non_string_key_raises_error);
    RUN_TEST(test_incr_non_number_delta_raises_error);
    RUN_TEST(test_incr_negative_delta_raises_error);

    /* decr */
    RUN_TEST(test_decr_with_delta);
    RUN_TEST(test_decr_default_delta);
    RUN_TEST(test_decr_no_args_raises_error);
    RUN_TEST(test_decr_negative_delta_raises_error);

    /* Integration */
    RUN_TEST(test_get_usable_as_request);
    RUN_TEST(test_set_usable_as_request);

    return UNITY_END();
}
