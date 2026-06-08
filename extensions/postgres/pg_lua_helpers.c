/*
 * pg_lua_helpers.c — PostgreSQL Lua helpers (ADR 0005, P6-1 + P6-2).
 *
 * Exposes pg.query(), pg.prepare(), and pg.execute() under the "postgres"
 * Lua namespace.  Registered via the extension API's register_helpers() call
 * in init.c — no direct script_engine dependency.
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/luajit/src/ lua headers, standard library.
 * NO src/ headers.
 */

#include "pg_lua_helpers.h"
#include "pg_message.h"

#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#define PG_MAX_PARAMS 64

/* -------------------------------------------------------------------------
 * pg.query(sql) -> wire bytes
 * ---------------------------------------------------------------------- */

static int lua_pg_query(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "pg.query: expected one string argument");

    size_t sql_len;
    const char *sql = lua_tolstring(L, 1, &sql_len);
    (void)sql_len;

    char buf[65536];
    int n = pg_encode_query(buf, sizeof(buf), sql);
    if (n <= 0)
        return luaL_error(L, "pg.query: SQL too large");

    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

/* -------------------------------------------------------------------------
 * pg.prepare(sql) -> {sql = sql}   (client-side opaque handle)
 * ---------------------------------------------------------------------- */

static int lua_pg_prepare(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "pg.prepare: expected one SQL string");
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "sql");
    return 1;
}

/* -------------------------------------------------------------------------
 * pg.execute(stmt, param1, ...) -> wire bytes
 *
 * Emits Parse("") + Bind("") + Describe('P', "") + Execute("") + Sync.
 * The anonymous prepared statement ("") is implicitly replaced by PostgreSQL
 * on each new Parse, so no per-connection parse-lifecycle tracking is needed.
 * The Describe('P') causes the server to emit RowDescription (or NoData),
 * exercising the pg_state.columns metadata storage path in postgres.c.
 * ---------------------------------------------------------------------- */

static int lua_pg_execute(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    int nargs = lua_gettop(L);
    if (nargs < 1)
        return luaL_error(L, "pg.execute: expected sql or pg.prepare() handle");

    const char *sql = NULL;
    if (lua_type(L, 1) == LUA_TSTRING) {
        sql = lua_tostring(L, 1);
    } else if (lua_type(L, 1) == LUA_TTABLE) {
        lua_getfield(L, 1, "sql");
        if (lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "pg.execute: invalid pg.prepare() handle");
        sql = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else {
        return luaL_error(L,
            "pg.execute: first arg must be SQL string or pg.prepare() handle");
    }

    int n_params = nargs - 1;
    if (n_params > PG_MAX_PARAMS)
        return luaL_error(L, "pg.execute: too many parameters (max %d)",
                          PG_MAX_PARAMS);

    const char *params[PG_MAX_PARAMS];
    size_t      param_lens[PG_MAX_PARAMS];

    for (int i = 0; i < n_params; i++) {
        int idx = i + 2;
        if (lua_isnil(L, idx)) {
            params[i]     = NULL;
            param_lens[i] = 0;
        } else if (lua_type(L, idx) == LUA_TSTRING ||
                   lua_type(L, idx) == LUA_TNUMBER) {
            params[i] = lua_tolstring(L, idx, &param_lens[i]);
        } else {
            return luaL_error(L,
                "pg.execute: param %d must be string, number, or nil", i + 1);
        }
    }

    char   buf[131072];
    size_t pos = 0;
    int    n;

    n = pg_encode_parse(buf + pos, sizeof(buf) - pos, "", sql);
    if (n <= 0) return luaL_error(L, "pg.execute: SQL too large");
    pos += (size_t)n;

    n = pg_encode_bind(buf + pos, sizeof(buf) - pos, "", "",
                       params, param_lens, (int16_t)n_params);
    if (n <= 0) return luaL_error(L, "pg.execute: parameter encoding failed");
    pos += (size_t)n;

    n = pg_encode_describe(buf + pos, sizeof(buf) - pos, 'P', "");
    if (n <= 0) return luaL_error(L, "pg.execute: buffer overflow");
    pos += (size_t)n;

    n = pg_encode_execute(buf + pos, sizeof(buf) - pos, "", 0);
    if (n <= 0) return luaL_error(L, "pg.execute: buffer overflow");
    pos += (size_t)n;

    n = pg_encode_sync(buf + pos, sizeof(buf) - pos);
    if (n <= 0) return luaL_error(L, "pg.execute: buffer overflow");
    pos += (size_t)n;

    lua_pushlstring(L, buf, pos);
    return 1;
}

/* -------------------------------------------------------------------------
 * Helper table
 * ---------------------------------------------------------------------- */

const script_helper postgres_lua_helpers[] = {
    { "query",   lua_pg_query   },
    { "prepare", lua_pg_prepare },
    { "execute", lua_pg_execute },
};

const size_t postgres_lua_helpers_count =
    sizeof(postgres_lua_helpers) / sizeof(postgres_lua_helpers[0]);
