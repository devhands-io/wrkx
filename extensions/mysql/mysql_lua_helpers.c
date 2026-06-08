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
 * Helper table
 * ---------------------------------------------------------------------- */

const script_helper mysql_lua_helpers[] = {
    { "query", lua_mysql_query },
};

const size_t mysql_lua_helpers_count =
    sizeof(mysql_lua_helpers) / sizeof(mysql_lua_helpers[0]);
