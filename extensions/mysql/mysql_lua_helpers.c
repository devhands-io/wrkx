/*
 * mysql_lua_helpers.c — MySQL Lua helpers (ADR 0005, P6-4).
 *
 * Exposes mysql.query(sql) under the "mysql" Lua namespace.
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/luajit/src/ lua headers, standard library.
 * NO src/ headers.
 */

#include "mysql_lua_helpers.h"
#include "mysql_packet.h"

#include <string.h>

#include <lua.h>
#include <lauxlib.h>

/* -------------------------------------------------------------------------
 * mysql.query(sql)
 *
 * Encodes a COM_QUERY packet and returns the wire bytes as a Lua string.
 * Mirrors redis.command() and pg.query() conventions.
 * ---------------------------------------------------------------------- */

static int lua_mysql_query(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "mysql.query: expected one string argument");

    size_t sql_len;
    const char *sql = lua_tolstring(L, 1, &sql_len);

    /* 4-byte header + COM_QUERY(1) + sql; max MySQL payload = 0xFFFFFF */
    uint8_t buf[65540];
    int n = mysql_encode_com_query(buf, sizeof(buf), sql, sql_len);
    if (n <= 0)
        return luaL_error(L, "mysql.query: SQL too large");

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

/* -------------------------------------------------------------------------
 * mysql.prepare(sql)
 *
 * Returns a Lua table {sql = sql} to be passed to mysql.execute().
 * Mirrors pg.prepare() conventions.
 * ---------------------------------------------------------------------- */

static int lua_mysql_prepare(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) < 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "mysql.prepare: expected one SQL string");

    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "sql");
    return 1;
}

/* -------------------------------------------------------------------------
 * mysql.execute(handle_or_sql, param1, ...)
 *
 * Encodes an internal "prepared execute" blob and returns it as a Lua string.
 * The first argument may be a SQL string or a mysql.prepare() handle table.
 * Params may be strings, numbers (coerced by lua_tolstring), or nil (NULL).
 * ---------------------------------------------------------------------- */

static int lua_mysql_execute(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    int nargs = lua_gettop(L);
    if (nargs < 1)
        return luaL_error(L, "mysql.execute: expected sql or mysql.prepare() handle");

    const char *sql     = NULL;
    size_t      sql_len = 0;

    if (lua_type(L, 1) == LUA_TSTRING) {
        sql = lua_tolstring(L, 1, &sql_len);
    } else if (lua_type(L, 1) == LUA_TTABLE) {
        lua_getfield(L, 1, "sql");
        if (lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "mysql.execute: invalid mysql.prepare() handle");
        sql = lua_tolstring(L, -1, &sql_len);
        lua_pop(L, 1);
    } else {
        return luaL_error(L, "mysql.execute: first arg must be SQL string or handle");
    }

    /* Enforce cache-key limit here so Lua sees an error early */
    if (sql_len > MYSQL_MAX_PREPARED_SQL)
        return luaL_error(L, "mysql.execute: SQL exceeds %d-byte limit",
                          MYSQL_MAX_PREPARED_SQL);

    int n_params = nargs - 1;
    if (n_params > 127)
        return luaL_error(L, "mysql.execute: too many parameters (max 127)");

    const char *params[128]; size_t param_lens[128];
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
                "mysql.execute: param %d must be string, number, or nil", i + 1);
        }
    }

    uint8_t buf[65540];   /* must match sizeof(s->pending) in mysql.c */
    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          sql, sql_len,
                                          params, param_lens, n_params);
    if (n <= 0)
        return luaL_error(L, "mysql.execute: SQL or params too large");

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

/* -------------------------------------------------------------------------
 * Helper table
 * ---------------------------------------------------------------------- */

const script_helper mysql_lua_helpers[] = {
    { "query",   lua_mysql_query   },
    { "prepare", lua_mysql_prepare },
    { "execute", lua_mysql_execute },
};

const size_t mysql_lua_helpers_count =
    sizeof(mysql_lua_helpers) / sizeof(mysql_lua_helpers[0]);
