/*
 * tests/unit/test_pg_lua.c
 *
 * Unit tests for the PostgreSQL Lua helper namespace (ADR 0005, P6-1 + P6-2).
 *
 * Tests pg.query(), pg.prepare(), and pg.execute() without a live PostgreSQL
 * server.  Mirrors the pattern from test_mc_lua.c.
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
#include "pg_lua_helpers.h"

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

    lua_register_helpers(engine, "pg",
                         postgres_lua_helpers, postgres_lua_helpers_count);

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

/* Call pg.<fn> with nargs args already on the stack.  Returns 0 on success. */
static int call_helper(const char *fn, int nargs) {
    lua_getglobal(L, "pg");
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    if (nargs > 0)
        lua_insert(L, -(nargs + 1));
    return lua_pcall(L, nargs, 1, 0);
}

/* Read bytes from a Lua string result at stack top; caller must pop. */
static const char *get_lstring(size_t *len) {
    return lua_tolstring(L, -1, len);
}

/* -------------------------------------------------------------------------
 * pg.query tests
 * ---------------------------------------------------------------------- */

void test_pg_query_still_works(void) {
    lua_pushstring(L, "SELECT 1");
    int err = call_helper("query", 1);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_EQUAL_CHAR('Q', buf[0]);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * pg.prepare tests
 * ---------------------------------------------------------------------- */

void test_pg_prepare_returns_table(void) {
    lua_pushstring(L, "SELECT 1");
    int err = call_helper("prepare", 1);
    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_TRUE(lua_istable(L, -1));

    lua_getfield(L, -1, "sql");
    TEST_ASSERT_EQUAL_STRING("SELECT 1", lua_tostring(L, -1));
    lua_pop(L, 2);
}

void test_pg_prepare_wrong_type_errors(void) {
    lua_pushnumber(L, 42);
    int err = call_helper("prepare", 1);
    TEST_ASSERT_NOT_EQUAL(0, err);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * pg.execute tests
 * ---------------------------------------------------------------------- */

void test_pg_execute_with_string_sql(void) {
    lua_pushstring(L, "SELECT $1::int");
    lua_pushstring(L, "42");
    int err = call_helper("execute", 2);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]); /* Parse tag */
    lua_pop(L, 1);
}

void test_pg_execute_with_prepare_handle(void) {
    /* Build handle via pg.prepare */
    lua_pushstring(L, "SELECT $1");
    int err = call_helper("prepare", 1);
    TEST_ASSERT_EQUAL_INT(0, err);
    /* handle is on stack; push param */
    lua_pushstring(L, "7");
    err = call_helper("execute", 2);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]);
    lua_pop(L, 1);

    /* Compare against direct string form */
    lua_pushstring(L, "SELECT $1");
    lua_pushstring(L, "7");
    err = call_helper("execute", 2);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len2;
    get_lstring(&len2);
    TEST_ASSERT_EQUAL_size_t(len, len2);
    lua_pop(L, 1);
}

void test_pg_execute_no_params(void) {
    lua_pushstring(L, "SELECT 1");
    int err = call_helper("execute", 1);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);
    /* Must contain Parse('P') + Bind('B') + Describe('D') + Execute('E') + Sync('S') */
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]);
    lua_pop(L, 1);
}

void test_pg_execute_describe_position(void) {
    /* Verify tag order: 'P', 'B', 'D' (with 'P' type byte), 'E', 'S' */
    lua_pushstring(L, "SELECT $1");
    lua_pushstring(L, "x");
    int err = call_helper("execute", 2);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);

    char tags[5];
    const char *p = buf;
    for (int i = 0; i < 5; i++) {
        tags[i] = p[0];
        uint32_t msg_len = ((uint32_t)(unsigned char)p[1] << 24) |
                           ((uint32_t)(unsigned char)p[2] << 16) |
                           ((uint32_t)(unsigned char)p[3] <<  8) |
                            (uint32_t)(unsigned char)p[4];
        p += 1 + msg_len;
    }

    TEST_ASSERT_EQUAL_CHAR('P', tags[0]);
    TEST_ASSERT_EQUAL_CHAR('B', tags[1]);
    TEST_ASSERT_EQUAL_CHAR('D', tags[2]);
    TEST_ASSERT_EQUAL_CHAR('E', tags[3]);
    TEST_ASSERT_EQUAL_CHAR('S', tags[4]);

    /* The Describe message type byte must be 'P' (portal describe) */
    /* Describe position: after Parse and Bind */
    const char *parse_start = buf;
    uint32_t parse_len = ((uint32_t)(unsigned char)parse_start[1] << 24) |
                         ((uint32_t)(unsigned char)parse_start[2] << 16) |
                         ((uint32_t)(unsigned char)parse_start[3] <<  8) |
                          (uint32_t)(unsigned char)parse_start[4];
    const char *bind_start = parse_start + 1 + parse_len;
    uint32_t bind_len = ((uint32_t)(unsigned char)bind_start[1] << 24) |
                        ((uint32_t)(unsigned char)bind_start[2] << 16) |
                        ((uint32_t)(unsigned char)bind_start[3] <<  8) |
                         (uint32_t)(unsigned char)bind_start[4];
    const char *desc_start = bind_start + 1 + bind_len;
    /* desc_start[5] is the type byte ('P' for portal) */
    TEST_ASSERT_EQUAL_CHAR('P', desc_start[5]);
    /* portal name is empty: desc_start[6] == '\0' */
    TEST_ASSERT_EQUAL_CHAR('\0', desc_start[6]);

    lua_pop(L, 1);
}

void test_pg_execute_null_param(void) {
    lua_pushstring(L, "INSERT INTO t VALUES($1)");
    lua_pushnil(L);
    int err = call_helper("execute", 2);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);

    /* Scan to the Bind message to find the null param encoding (int32 = -1) */
    /* Skip Parse: buf[0]='P', skip 1 + rd_i32(buf+1) bytes */
    uint32_t parse_len = ((uint32_t)(unsigned char)buf[1] << 24) |
                         ((uint32_t)(unsigned char)buf[2] << 16) |
                         ((uint32_t)(unsigned char)buf[3] <<  8) |
                          (uint32_t)(unsigned char)buf[4];
    const char *bind = buf + 1 + parse_len;
    TEST_ASSERT_EQUAL_CHAR('B', bind[0]);
    /* Bind body: portal\0 + stmt\0 + int16(0) + int16(1) + int32(-1) + int16(0)
     *          = 1       + 1      + 2         + 2         + 4         + 2
     * So n_params offset = 5 (tag) + portal + stmt + 2 fmtcodes + 2 = 1+4+1+1+2 */
    /* portal at bind[5]='\0', stmt at bind[6]='\0', fmt=bind[7..8],
     * n_params=bind[9..10], first param at bind[11..14] */
    int32_t first_len = (int32_t)(((uint32_t)(unsigned char)bind[11] << 24) |
                                  ((uint32_t)(unsigned char)bind[12] << 16) |
                                  ((uint32_t)(unsigned char)bind[13] <<  8) |
                                   (uint32_t)(unsigned char)bind[14]);
    TEST_ASSERT_EQUAL_INT32(-1, first_len);
    lua_pop(L, 1);
}

void test_pg_execute_number_param(void) {
    lua_pushstring(L, "SELECT $1");
    lua_pushnumber(L, 3.14);
    int err = call_helper("execute", 2);
    TEST_ASSERT_EQUAL_INT(0, err);
    size_t len;
    const char *buf = get_lstring(&len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_GREATER_THAN(0, (int)len);
    lua_pop(L, 1);
}

void test_pg_execute_too_many_params_errors(void) {
    lua_pushstring(L, "SELECT 1");
    for (int i = 0; i < 65; i++)    /* PG_MAX_PARAMS + 1 = 65 */
        lua_pushstring(L, "x");
    int err = call_helper("execute", 66);
    TEST_ASSERT_NOT_EQUAL(0, err);
    lua_pop(L, 1);
}

void test_pg_execute_invalid_handle_errors(void) {
    lua_newtable(L);  /* empty table — not a valid pg.prepare() handle */
    int err = call_helper("execute", 1);
    TEST_ASSERT_NOT_EQUAL(0, err);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_pg_query_still_works);

    RUN_TEST(test_pg_prepare_returns_table);
    RUN_TEST(test_pg_prepare_wrong_type_errors);

    RUN_TEST(test_pg_execute_with_string_sql);
    RUN_TEST(test_pg_execute_with_prepare_handle);
    RUN_TEST(test_pg_execute_no_params);
    RUN_TEST(test_pg_execute_describe_position);
    RUN_TEST(test_pg_execute_null_param);
    RUN_TEST(test_pg_execute_number_param);
    RUN_TEST(test_pg_execute_too_many_params_errors);
    RUN_TEST(test_pg_execute_invalid_handle_errors);

    return UNITY_END();
}
