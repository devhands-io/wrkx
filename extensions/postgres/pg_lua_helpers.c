/*
 * pg_lua_helpers.c — PostgreSQL Lua helpers (ADR 0005, P6-1 + P6-2 + P6-3).
 *
 * Exposes pg.query(), pg.prepare(), pg.execute(), pg.result(), pg.begin(),
 * pg.commit(), and pg.rollback() under the "postgres" Lua namespace.
 *
 * Also provides the thread-local result storage (tls_result) and the
 * pg_result_*() helper functions used by postgres.c and pg_quickjs_helpers.c.
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/luajit/src/ lua headers, standard library.
 * NO src/ headers.
 */

#include "pg_lua_helpers.h"
#include "pg_message.h"
#include "pg_result.h"

#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#define PG_MAX_PARAMS 64

/* -------------------------------------------------------------------------
 * Thread-local result storage (one instance per OS thread)
 * ---------------------------------------------------------------------- */

__thread pg_result_t tls_result;

/* -------------------------------------------------------------------------
 * Result helper functions (called from postgres.c and qjs helpers)
 * ---------------------------------------------------------------------- */

void pg_result_reset(void) {
    tls_result.nrows      = 0;
    tls_result.ncols      = 0;
    tls_result.heap_used  = 0;
    tls_result.valid      = false;
    tls_result.cmd_tag[0] = '\0';
    tls_result.pg_status  = 0;
}

void pg_result_set_columns(const pg_col_desc_t *cols, int16_t ncols) {
    tls_result.ncols = ncols;
    if (ncols > 0)
        memcpy(tls_result.cols, cols, (size_t)ncols * sizeof(pg_col_desc_t));
}

void pg_result_append_row(const pg_col_desc_t *cols, int16_t ncols,
                          const pg_data_row_t *row, size_t consumed) {
    (void)consumed;
    if (tls_result.nrows >= PG_RESULT_MAX_ROWS) return;

    int32_t r = tls_result.nrows;
    int16_t store = (row->nfields < ncols) ? row->nfields : ncols;

    for (int16_t c = 0; c < store; c++) {
        int32_t flen = row->fields[c].len;
        if (flen < 0 || row->fields[c].data == NULL) {
            tls_result.fields[r][c].value = NULL;
            tls_result.fields[r][c].len   = 0;
        } else {
            size_t need = (size_t)flen;
            if (tls_result.heap_used + need > PG_RESULT_HEAP_SIZE) {
                /* heap full — store NULL to signal truncation */
                tls_result.fields[r][c].value = NULL;
                tls_result.fields[r][c].len   = 0;
            } else {
                char *dst = tls_result.heap + tls_result.heap_used;
                memcpy(dst, row->fields[c].data, need);
                tls_result.heap_used += need;
                tls_result.fields[r][c].value = dst;
                tls_result.fields[r][c].len   = need;
            }
        }
    }
    /* zero remaining columns (for queries with fewer fields than cols) */
    for (int16_t c = store; c < ncols; c++) {
        tls_result.fields[r][c].value = NULL;
        tls_result.fields[r][c].len   = 0;
    }

    tls_result.nrows++;
    (void)cols;
}

void pg_result_set_cmd_tag(const char *tag) {
    size_t tlen = strlen(tag);
    size_t copy = tlen < sizeof(tls_result.cmd_tag) - 1
                ? tlen : sizeof(tls_result.cmd_tag) - 1;
    memcpy(tls_result.cmd_tag, tag, copy);
    tls_result.cmd_tag[copy] = '\0';
}

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
 * pg.result() -> table or nil
 * Valid only inside response(); returns nil if no result is available.
 * ---------------------------------------------------------------------- */

static int lua_pg_result(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (!tls_result.valid) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);

    lua_pushinteger(L, tls_result.ncols);
    lua_setfield(L, -2, "ncols");

    lua_pushinteger(L, tls_result.nrows);
    lua_setfield(L, -2, "nrows");

    lua_pushstring(L, tls_result.cmd_tag);
    lua_setfield(L, -2, "cmd_tag");

    char status_str[2] = { (char)tls_result.pg_status, '\0' };
    lua_pushstring(L, status_str);
    lua_setfield(L, -2, "status");

    lua_newtable(L);
    for (int c = 0; c < tls_result.ncols; c++) {
        lua_pushstring(L, tls_result.cols[c].name);
        lua_rawseti(L, -2, c + 1);
    }
    lua_setfield(L, -2, "cols");

    lua_newtable(L);
    for (int r = 0; r < tls_result.nrows; r++) {
        lua_newtable(L);
        for (int c = 0; c < tls_result.ncols; c++) {
            const char *v = tls_result.fields[r][c].value;
            if (v == NULL) {
                lua_pushnil(L);
            } else {
                lua_pushlstring(L, v, tls_result.fields[r][c].len);
            }
            lua_rawseti(L, -2, c + 1);
        }
        lua_rawseti(L, -2, r + 1);
    }
    lua_setfield(L, -2, "rows");

    return 1;
}

/* -------------------------------------------------------------------------
 * Transaction helpers
 * ---------------------------------------------------------------------- */

static int lua_pg_begin(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    char buf[16];
    int n = pg_encode_query(buf, sizeof(buf), "BEGIN");
    if (n <= 0) return luaL_error(L, "pg.begin: encode failed");
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

static int lua_pg_commit(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    char buf[16];
    int n = pg_encode_query(buf, sizeof(buf), "COMMIT");
    if (n <= 0) return luaL_error(L, "pg.commit: encode failed");
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

static int lua_pg_rollback(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    char buf[16];
    int n = pg_encode_query(buf, sizeof(buf), "ROLLBACK");
    if (n <= 0) return luaL_error(L, "pg.rollback: encode failed");
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

/* -------------------------------------------------------------------------
 * Helper table
 * ---------------------------------------------------------------------- */

const script_helper postgres_lua_helpers[] = {
    { "query",    lua_pg_query    },
    { "prepare",  lua_pg_prepare  },
    { "execute",  lua_pg_execute  },
    { "result",   lua_pg_result   },
    { "begin",    lua_pg_begin    },
    { "commit",   lua_pg_commit   },
    { "rollback", lua_pg_rollback },
};

const size_t postgres_lua_helpers_count =
    sizeof(postgres_lua_helpers) / sizeof(postgres_lua_helpers[0]);
