/*
 * tests/unit/test_mysql_lua.c
 *
 * Unit tests for the MySQL Lua helper namespace (ADR 0005, P6-4).
 *
 * Tests mysql.query() without a live MySQL server.
 * Mirrors the pattern from test_mc_lua.c and test_pg_lua.c.
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
#include "mysql_lua_helpers.h"
#include "mysql_packet.h"

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

    lua_register_helpers(engine, "mysql",
                         mysql_lua_helpers, mysql_lua_helpers_count);

    L = (lua_State *)lua_engine_state(engine);
    TEST_ASSERT_NOT_NULL(L);
}

void tearDown(void) {
    api->destroy(engine);
    engine = NULL;
    L      = NULL;
}

/* -------------------------------------------------------------------------
 * Helper: run a Lua chunk; return top-of-stack string
 * ---------------------------------------------------------------------- */

static int run_chunk(const char *chunk) {
    return luaL_dostring(L, chunk);
}

/* Run chunk; expect it to leave one string on the stack.
   Returns pointer to the Lua-managed string and its length. */
static const char *run_and_get_str(const char *chunk, size_t *len) {
    lua_settop(L, 0);
    int rc = run_chunk(chunk);
    if (rc != LUA_OK) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
        return NULL;
    }
    if (lua_type(L, -1) != LUA_TSTRING) return NULL;
    return lua_tolstring(L, -1, len);
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

void test_mysql_query_select1(void) {
    size_t len;
    const char *s = run_and_get_str("return mysql.query('SELECT 1')", &len);
    TEST_ASSERT_NOT_NULL(s);
    /* Wire: 4-byte header + COM_QUERY(0x03) + "SELECT 1" = 13 bytes */
    TEST_ASSERT_EQUAL_size_t(13, len);
    TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)s[4]);   /* COM_QUERY tag */
    TEST_ASSERT_EQUAL_MEMORY("SELECT 1", s + 5, 8);
}

void test_mysql_query_header_seq_zero(void) {
    size_t len;
    const char *s = run_and_get_str("return mysql.query('SELECT 1')", &len);
    TEST_ASSERT_NOT_NULL(s);
    /* 4-byte header: bytes [0..2] = payload_len (LE), byte [3] = seq */
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)s[3]);   /* seq=0 */
    /* payload length = 9 (1 + 8) */
    uint32_t plen = (uint8_t)s[0] | ((uint8_t)s[1]<<8) | ((uint8_t)s[2]<<16);
    TEST_ASSERT_EQUAL_UINT32(9, plen);
}

void test_mysql_query_wrong_type_errors(void) {
    lua_settop(L, 0);
    int rc = run_chunk("return mysql.query(42)");
    TEST_ASSERT_NOT_EQUAL(LUA_OK, rc);   /* must raise a Lua error */
}

void test_mysql_query_empty_sql(void) {
    size_t len;
    const char *s = run_and_get_str("return mysql.query('')", &len);
    TEST_ASSERT_NOT_NULL(s);
    /* 4-byte header + COM_QUERY(1) + nothing = 5 bytes */
    TEST_ASSERT_EQUAL_size_t(5, len);
    TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)s[4]);
}

void test_mysql_query_large_sql(void) {
    size_t len;
    const char *s = run_and_get_str(
        "return mysql.query(string.rep('x', 16384))", &len);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_size_t(4 + 1 + 16384, len);
    /* payload length field should reflect 16385 */
    uint32_t plen = (uint8_t)s[0] | ((uint8_t)s[1]<<8) | ((uint8_t)s[2]<<16);
    TEST_ASSERT_EQUAL_UINT32(16385, plen);
}

void test_mysql_query_too_large_errors(void) {
    /* 65536 bytes of SQL overflows the 65535-byte payload limit */
    lua_settop(L, 0);
    int rc = run_chunk("return mysql.query(string.rep('x', 65536))");
    TEST_ASSERT_NOT_EQUAL(LUA_OK, rc);
}

/* -------------------------------------------------------------------------
 * mysql.execute tests
 * ---------------------------------------------------------------------- */

static void decode_blob(const char *blob, size_t blen,
                        const char **params, size_t *lens, int *np) {
    int rc = mysql_decode_prepared_request_params(
                 (const uint8_t *)blob, blen, params, lens, np);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void test_mysql_execute_no_params(void) {
    size_t len;
    const char *s = run_and_get_str("return mysql.execute('SELECT 1')", &len);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(1,
        mysql_is_prepared_request((const uint8_t *)s, len));
    const char *params[128]; size_t plens[128]; int np;
    decode_blob(s, len, params, plens, &np);
    TEST_ASSERT_EQUAL_INT(0, np);
}

void test_mysql_execute_with_string_param(void) {
    size_t len;
    const char *s = run_and_get_str(
        "return mysql.execute('SELECT ?', 'hello')", &len);
    TEST_ASSERT_NOT_NULL(s);
    const char *params[128]; size_t plens[128]; int np;
    decode_blob(s, len, params, plens, &np);
    TEST_ASSERT_EQUAL_INT(1, np);
    TEST_ASSERT_NOT_NULL(params[0]);
    TEST_ASSERT_EQUAL_size_t(5, plens[0]);
    TEST_ASSERT_EQUAL_MEMORY("hello", params[0], 5);
}

void test_mysql_execute_with_number_param(void) {
    size_t len;
    const char *s = run_and_get_str(
        "return mysql.execute('SELECT ?', 42)", &len);
    TEST_ASSERT_NOT_NULL(s);
    const char *params[128]; size_t plens[128]; int np;
    decode_blob(s, len, params, plens, &np);
    TEST_ASSERT_EQUAL_INT(1, np);
    TEST_ASSERT_NOT_NULL(params[0]);
    /* lua_tolstring coerces 42 → "42" */
    TEST_ASSERT_EQUAL_size_t(2, plens[0]);
    TEST_ASSERT_EQUAL_MEMORY("42", params[0], 2);
}

void test_mysql_execute_with_null_param(void) {
    size_t len;
    const char *s = run_and_get_str(
        "return mysql.execute('SELECT ?', nil)", &len);
    TEST_ASSERT_NOT_NULL(s);
    const char *params[128]; size_t plens[128]; int np;
    decode_blob(s, len, params, plens, &np);
    TEST_ASSERT_EQUAL_INT(1, np);
    TEST_ASSERT_NULL(params[0]);
}

void test_mysql_execute_from_prepare_handle(void) {
    size_t len;
    const char *s = run_and_get_str(
        "local h = mysql.prepare('SELECT ?') "
        "return mysql.execute(h, 'x')", &len);
    TEST_ASSERT_NOT_NULL(s);
    const char *params[128]; size_t plens[128]; int np;
    decode_blob(s, len, params, plens, &np);
    TEST_ASSERT_EQUAL_INT(1, np);
    TEST_ASSERT_NOT_NULL(params[0]);
    TEST_ASSERT_EQUAL_size_t(1, plens[0]);
    TEST_ASSERT_EQUAL_MEMORY("x", params[0], 1);
}

void test_mysql_execute_wrong_type_errors(void) {
    lua_settop(L, 0);
    int rc = run_chunk("return mysql.execute(42)");
    TEST_ASSERT_NOT_EQUAL(LUA_OK, rc);
}

void test_mysql_execute_too_many_params(void) {
    /* Build a call with 128 parameters (max is 127) */
    lua_settop(L, 0);
    /* Use a loop to create the call */
    int rc = run_chunk(
        "local args = {}\n"
        "for i = 1, 128 do args[i] = 'x' end\n"
        "return mysql.execute('SELECT 1', table.unpack(args))");
    TEST_ASSERT_NOT_EQUAL(LUA_OK, rc);
}

void test_mysql_execute_sql_at_limit(void) {
    /* 1023 bytes — exactly at the limit, must succeed */
    size_t len;
    const char *s = run_and_get_str(
        "return mysql.execute(string.rep('x', 1023))", &len);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(1,
        mysql_is_prepared_request((const uint8_t *)s, len));
}

void test_mysql_execute_sql_over_limit(void) {
    /* 1024 bytes — one over the limit, must error */
    lua_settop(L, 0);
    int rc = run_chunk("return mysql.execute(string.rep('x', 1024))");
    TEST_ASSERT_NOT_EQUAL(LUA_OK, rc);
    /* Error message should mention the limit */
    const char *errmsg = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(errmsg);
    TEST_ASSERT_NOT_NULL(strstr(errmsg, "SQL exceeds"));
}

void test_mysql_prepare_still_works(void) {
    lua_settop(L, 0);
    int rc = run_chunk("local h = mysql.prepare('SELECT 1') "
                       "return h.sql");
    TEST_ASSERT_EQUAL_INT(LUA_OK, rc);
    TEST_ASSERT_EQUAL_INT(LUA_TSTRING, lua_type(L, -1));
    size_t len;
    const char *sql = lua_tolstring(L, -1, &len);
    TEST_ASSERT_EQUAL_MEMORY("SELECT 1", sql, 8);
}

void test_mysql_query_still_works(void) {
    size_t len;
    const char *s = run_and_get_str("return mysql.query('SELECT 1')", &len);
    TEST_ASSERT_NOT_NULL(s);
    /* COM_QUERY: byte[4] = 0x03, NOT a prepared blob */
    TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)s[4]);
    TEST_ASSERT_EQUAL_INT(0,
        mysql_is_prepared_request((const uint8_t *)s, len));
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_mysql_query_select1);
    RUN_TEST(test_mysql_query_header_seq_zero);
    RUN_TEST(test_mysql_query_wrong_type_errors);
    RUN_TEST(test_mysql_query_empty_sql);
    RUN_TEST(test_mysql_query_large_sql);
    RUN_TEST(test_mysql_query_too_large_errors);

    RUN_TEST(test_mysql_execute_no_params);
    RUN_TEST(test_mysql_execute_with_string_param);
    RUN_TEST(test_mysql_execute_with_number_param);
    RUN_TEST(test_mysql_execute_with_null_param);
    RUN_TEST(test_mysql_execute_from_prepare_handle);
    RUN_TEST(test_mysql_execute_wrong_type_errors);
    RUN_TEST(test_mysql_execute_too_many_params);
    RUN_TEST(test_mysql_execute_sql_at_limit);
    RUN_TEST(test_mysql_execute_sql_over_limit);
    RUN_TEST(test_mysql_prepare_still_works);
    RUN_TEST(test_mysql_query_still_works);

    return UNITY_END();
}
