/* tests/unit/test_redis_lua.c
 *
 * Unit tests for the Redis Lua glue module (ADR 0005, Phase 2, P2-2).
 *
 * Tests the "redis" Lua namespace registered by redis_helpers.c:
 *   - redis.command() returns correctly RESP-encoded bytes for each command
 *   - Error cases raise Lua errors (caught by pcall), not crashes
 *   - redis.pipeline() is a stub that raises "not yet implemented"
 *
 * Uses the same engine-creation pattern as test_lua_engine.c.
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
#include "redis_lua_helpers.h"   /* from extensions/redis/ */
#include "resp.h"                /* from extensions/redis/ — to verify RESP encoding */

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

    /* Register Redis helpers after init (globals persist). */
    lua_register_helpers(engine, "redis",
                         redis_lua_helpers, redis_lua_helpers_count);

    L = (lua_State *) lua_engine_state(engine);
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

/*
 * Call redis.command(arg, ...) with the given argv via the Lua C API.
 * On success: pushes the result string; returns the lua_State stack top (1).
 * On error:   uses TEST_FAIL_MESSAGE with the error from the Lua stack.
 * Caller must lua_pop(L, 1) after consuming the result.
 */
static void call_redis_command(int argc, ...) {
    va_list ap;
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "command");
    lua_remove(L, -2);  /* remove "redis" table */

    va_start(ap, argc);
    for (int i = 0; i < argc; i++)
        lua_pushstring(L, va_arg(ap, const char *));
    va_end(ap);

    if (lua_pcall(L, argc, 1, 0) != 0) {
        const char *err = lua_tostring(L, -1);
        lua_pop(L, 1);
        TEST_FAIL_MESSAGE(err);
    }
}

/* -------------------------------------------------------------------------
 * redis.command() — encoding correctness
 * ---------------------------------------------------------------------- */

void test_command_ping_encodes_correctly(void) {
    call_redis_command(1, "PING");

    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "*1\r\n$4\r\nPING\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), (int)len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_command_set_encodes_correctly(void) {
    call_redis_command(3, "SET", "k", "v");

    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), (int)len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_command_get_encodes_correctly(void) {
    call_redis_command(2, "GET", "mykey");

    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), (int)len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_command_incr_encodes_correctly(void) {
    call_redis_command(2, "INCR", "counter");

    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    const char *expected = "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), (int)len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result, len);
    lua_pop(L, 1);
}

void test_command_result_is_valid_resp(void) {
    /* Any command should produce bytes parseable by resp_parse as incomplete
     * (need server reply) — but the REQUEST bytes themselves are not a RESP
     * response. Verify the bytes start with '*' (bulk array prefix). */
    call_redis_command(2, "HGET", "myhash", "field");

    (void)0; /* suppress unused-result warning */
    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_CHAR('*', result[0]);
    lua_pop(L, 1);
}

void test_command_multiarg_encodes_correctly(void) {
    /* MSET k1 v1 k2 v2 — four args after the command */
    call_redis_command(5, "MSET", "k1", "v1", "k2", "v2");

    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    /* Must start with *5 for 5 args */
    TEST_ASSERT_EQUAL_CHAR('*', result[0]);
    TEST_ASSERT_EQUAL_CHAR('5', result[1]);
    TEST_ASSERT_TRUE(len > 0);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * redis.command() — error cases (must raise Lua error, not crash)
 * ---------------------------------------------------------------------- */

void test_command_no_args_raises_error(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "command");
    lua_remove(L, -2);

    int rc = lua_pcall(L, 0, 1, 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);  /* must error */
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_NOT_NULL(strstr(err, "at least one argument"));
    lua_pop(L, 1);
}

void test_command_non_string_arg_raises_error(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "command");
    lua_remove(L, -2);
    lua_pushstring(L, "SET");
    lua_pushnumber(L, 42.0);  /* non-string second arg */

    int rc = lua_pcall(L, 2, 1, 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_NOT_NULL(strstr(err, "must be a string"));
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * redis.pipeline() — no-args error (previously tested stub message)
 * ---------------------------------------------------------------------- */

void test_pipeline_stub_raises_error(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "pipeline");
    lua_remove(L, -2);

    int rc = lua_pcall(L, 0, 0, 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    const char *err = lua_tostring(L, -1);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_NOT_NULL(strstr(err, "at least one command required"));
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * Namespace registration
 * ---------------------------------------------------------------------- */

void test_redis_namespace_exists(void) {
    lua_getglobal(L, "redis");
    TEST_ASSERT_TRUE(lua_istable(L, -1));
    lua_pop(L, 1);
}

void test_redis_command_is_callable(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "command");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

void test_redis_pipeline_is_callable(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "pipeline");
    TEST_ASSERT_TRUE(lua_isfunction(L, -1));
    lua_pop(L, 2);
}

/* -------------------------------------------------------------------------
 * redis.pipeline() — concatenation and error cases
 * ---------------------------------------------------------------------- */

void test_pipeline_single_command(void) {
    /* redis.pipeline(redis.command("PING")) == redis.command("PING") */
    call_redis_command(1, "PING");
    size_t cmd_len;
    const char *cmd_bytes = lua_tolstring(L, -1, &cmd_len);
    char *cmd_copy = malloc(cmd_len);
    memcpy(cmd_copy, cmd_bytes, cmd_len);
    lua_pop(L, 1);

    /* Now call redis.pipeline() with one pre-encoded string. */
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "pipeline");
    lua_remove(L, -2);
    lua_pushlstring(L, cmd_copy, cmd_len);
    TEST_ASSERT_EQUAL_INT(0, lua_pcall(L, 1, 1, 0));
    size_t pipe_len;
    const char *pipe_bytes = lua_tolstring(L, -1, &pipe_len);
    TEST_ASSERT_EQUAL_UINT(cmd_len, pipe_len);
    TEST_ASSERT_EQUAL_MEMORY(cmd_copy, pipe_bytes, pipe_len);
    lua_pop(L, 1);
    free(cmd_copy);
}

void test_pipeline_two_commands_concatenated(void) {
    /* redis.pipeline(cmd1, cmd2) should equal cmd1 .. cmd2 in Lua. */
    const char *ping_resp = "*1\r\n$4\r\nPING\r\n";
    const char *set_resp  = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    size_t total = strlen(ping_resp) + strlen(set_resp);

    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "pipeline");
    lua_remove(L, -2);
    lua_pushstring(L, ping_resp);
    lua_pushstring(L, set_resp);
    TEST_ASSERT_EQUAL_INT(0, lua_pcall(L, 2, 1, 0));

    size_t len;
    const char *result = lua_tolstring(L, -1, &len);
    TEST_ASSERT_EQUAL_UINT(total, len);
    TEST_ASSERT_EQUAL_MEMORY(ping_resp, result, strlen(ping_resp));
    TEST_ASSERT_EQUAL_MEMORY(set_resp, result + strlen(ping_resp), strlen(set_resp));
    lua_pop(L, 1);
}

void test_pipeline_no_args_raises_error(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "pipeline");
    lua_remove(L, -2);
    int rc = lua_pcall(L, 0, 1, 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    lua_pop(L, 1);
}

void test_pipeline_non_string_arg_raises_error(void) {
    lua_getglobal(L, "redis");
    lua_getfield(L, -1, "pipeline");
    lua_remove(L, -2);
    lua_pushnumber(L, 42.0);
    int rc = lua_pcall(L, 1, 1, 0);
    TEST_ASSERT_NOT_EQUAL(0, rc);
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * Integration: command bytes usable as request() return value
 * ---------------------------------------------------------------------- */

void test_command_bytes_usable_as_request(void) {
    /* Simulate: function request() return redis.command("PING") end */
    const char *script =
        "function request()\n"
        "    return redis.command('PING')\n"
        "end\n";
    int rc = luaL_dostring(L, script);
    if (rc != 0) {
        TEST_FAIL_MESSAGE(lua_tostring(L, -1));
    }

    size_t len = 0;
    char *buf = api->request(engine, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_CHAR('*', buf[0]);
    const char *expected = "*1\r\n$4\r\nPING\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), (int)len);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, len);
    free(buf);
}

/* -------------------------------------------------------------------------
 * extension @lua tagging audit (ADR 0005, Phase 5, t069)
 *
 * The Redis helper bodies cast engine_ctx to lua_State *, so the extension must
 * register them under "redis@lua" — the host then binds them only to the LuaJIT
 * engine and never into a foreign VM.
 * ---------------------------------------------------------------------- */

void wrkx_extension_init_redis(const wrkx_extension_api *ext_api);

static const char *audit_ns;

static void audit_register_protocol(const protocol *p) { (void)p; }
static void audit_register_helpers(const char *ns, const script_helper *h,
                                   size_t count) {
    (void)h; (void)count;
    audit_ns = ns;
}
static void audit_register_schema(const char *s, const char *st,
                                  const char *dp, wrkx_configure_fn c) {
    (void)s; (void)st; (void)dp; (void)c;
}

void test_redis_init_tags_helpers_lua(void) {
    audit_ns = NULL;
    wrkx_extension_api ext = {
        .version           = WRKX_EXTENSION_API_VERSION,
        .register_protocol = audit_register_protocol,
        .register_helpers  = audit_register_helpers,
        .register_schema   = audit_register_schema,
    };
    wrkx_extension_init_redis(&ext);

    TEST_ASSERT_NOT_NULL(audit_ns);
    TEST_ASSERT_EQUAL_STRING("redis@lua", audit_ns);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    /* Namespace registration */
    RUN_TEST(test_redis_namespace_exists);
    RUN_TEST(test_redis_command_is_callable);
    RUN_TEST(test_redis_pipeline_is_callable);

    /* Encoding correctness */
    RUN_TEST(test_command_ping_encodes_correctly);
    RUN_TEST(test_command_set_encodes_correctly);
    RUN_TEST(test_command_get_encodes_correctly);
    RUN_TEST(test_command_incr_encodes_correctly);
    RUN_TEST(test_command_result_is_valid_resp);
    RUN_TEST(test_command_multiarg_encodes_correctly);

    /* Error cases */
    RUN_TEST(test_command_no_args_raises_error);
    RUN_TEST(test_command_non_string_arg_raises_error);
    RUN_TEST(test_pipeline_stub_raises_error);

    /* redis.pipeline() */
    RUN_TEST(test_pipeline_single_command);
    RUN_TEST(test_pipeline_two_commands_concatenated);
    RUN_TEST(test_pipeline_no_args_raises_error);
    RUN_TEST(test_pipeline_non_string_arg_raises_error);

    /* Integration */
    RUN_TEST(test_command_bytes_usable_as_request);

    /* Extension @lua tagging audit */
    RUN_TEST(test_redis_init_tags_helpers_lua);

    return UNITY_END();
}
