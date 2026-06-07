/*
 * mc_lua_helpers.c — memcached Lua helper namespace.
 *
 * ADR 0005, Phase 4, t061.
 *
 * Exposes five helpers under the "memcached" Lua global table:
 *   memcached.get(key)                  → encoded get bytes
 *   memcached.set(key, value [, opts])  → encoded set bytes
 *   memcached.delete(key)               → encoded delete bytes
 *   memcached.incr(key [, delta])       → encoded incr bytes (delta ≥ 0)
 *   memcached.decr(key [, delta])       → encoded decr bytes (delta ≥ 0)
 *
 * Each helper returns the raw wire bytes to be used as the request() return
 * value.  Response handling happens in the protocol vtable (t062+).
 *
 * opts table fields (all optional, default to 0):
 *   flags   — uint32 client-managed flags
 *   exptime — uint32 TTL in seconds (0 = never expire)
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/luajit/src/ Lua headers, standard library.  NO src/ headers.
 */

#include "mc_lua_helpers.h"
#include "mc_request.h"

#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

/* -------------------------------------------------------------------------
 * memcached.get(key) -> string
 * ---------------------------------------------------------------------- */

static int lua_mc_get(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;

    if (lua_gettop(L) < 1)
        return luaL_error(L, "memcached.get: key argument required");
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "memcached.get: key must be a string");

    size_t      keylen;
    const char *key = lua_tolstring(L, 1, &keylen);

    mc_request req = {0};
    req.op     = MC_OP_GET;
    req.key    = key;
    req.keylen = keylen;

    char buf[512];
    int n = mc_request_encode(&req, buf, sizeof(buf));
    if (n < 0)
        return luaL_error(L, "memcached.get: failed to encode "
                             "(key too long or contains invalid characters)");

    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

/* -------------------------------------------------------------------------
 * memcached.set(key, value [, opts]) -> string
 *   opts = { flags = <uint32>, exptime = <uint32> }
 * ---------------------------------------------------------------------- */

static int lua_mc_set(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    int argc = lua_gettop(L);

    if (argc < 2)
        return luaL_error(L, "memcached.set: key and value required");
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "memcached.set: key must be a string");
    if (lua_type(L, 2) != LUA_TSTRING)
        return luaL_error(L, "memcached.set: value must be a string");

    size_t      keylen, vallen;
    const char *key = lua_tolstring(L, 1, &keylen);
    const char *val = lua_tolstring(L, 2, &vallen);

    uint32_t flags = 0, exptime = 0;
    if (argc >= 3) {
        if (lua_type(L, 3) != LUA_TTABLE)
            return luaL_error(L, "memcached.set: opts must be a table");

        lua_getfield(L, 3, "flags");
        if (!lua_isnil(L, -1)) flags = (uint32_t)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "exptime");
        if (!lua_isnil(L, -1)) exptime = (uint32_t)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }

    mc_request req = {0};
    req.op      = MC_OP_SET;
    req.key     = key;
    req.keylen  = keylen;
    req.value   = val;
    req.vallen  = vallen;
    req.flags   = flags;
    req.exptime = exptime;

    /* Header overhead: "set " + key(250) + " 4294967295 4294967295 " +
     * digits(vallen) + "\r\n" + "\r\n" ≈ 300 bytes + vallen. */
    size_t cap = 300 + vallen + 1;
    char  *buf = (char *)malloc(cap);
    if (!buf)
        return luaL_error(L, "memcached.set: out of memory");

    int n = mc_request_encode(&req, buf, cap);
    if (n < 0) {
        free(buf);
        return luaL_error(L, "memcached.set: failed to encode "
                             "(key too long or contains invalid characters)");
    }

    lua_pushlstring(L, buf, (size_t)n);
    free(buf);
    return 1;
}

/* -------------------------------------------------------------------------
 * memcached.delete(key) -> string
 * ---------------------------------------------------------------------- */

static int lua_mc_delete(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;

    if (lua_gettop(L) < 1)
        return luaL_error(L, "memcached.delete: key argument required");
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "memcached.delete: key must be a string");

    size_t      keylen;
    const char *key = lua_tolstring(L, 1, &keylen);

    mc_request req = {0};
    req.op     = MC_OP_DELETE;
    req.key    = key;
    req.keylen = keylen;

    char buf[512];
    int n = mc_request_encode(&req, buf, sizeof(buf));
    if (n < 0)
        return luaL_error(L, "memcached.delete: failed to encode "
                             "(key too long or contains invalid characters)");

    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

/* -------------------------------------------------------------------------
 * Shared helper for incr / decr
 * ---------------------------------------------------------------------- */

static int lua_mc_counter(void *engine_ctx, mc_op op, const char *name) {
    lua_State *L = (lua_State *)engine_ctx;
    int argc = lua_gettop(L);

    if (argc < 1)
        return luaL_error(L, "memcached.%s: key argument required", name);
    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "memcached.%s: key must be a string", name);

    size_t      keylen;
    const char *key = lua_tolstring(L, 1, &keylen);

    uint64_t delta = 1;
    if (argc >= 2) {
        if (lua_type(L, 2) != LUA_TNUMBER)
            return luaL_error(L, "memcached.%s: delta must be a number", name);
        lua_Number d = lua_tonumber(L, 2);
        if (d < 0)
            return luaL_error(L,
                "memcached.%s: delta must not be negative", name);
        delta = (uint64_t)d;
    }

    mc_request req = {0};
    req.op     = op;
    req.key    = key;
    req.keylen = keylen;
    req.delta  = delta;

    char buf[512];
    int n = mc_request_encode(&req, buf, sizeof(buf));
    if (n < 0)
        return luaL_error(L, "memcached.%s: failed to encode", name);

    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

static int lua_mc_incr(void *engine_ctx) {
    return lua_mc_counter(engine_ctx, MC_OP_INCR, "incr");
}

static int lua_mc_decr(void *engine_ctx) {
    return lua_mc_counter(engine_ctx, MC_OP_DECR, "decr");
}

/* -------------------------------------------------------------------------
 * Public helper table
 * ---------------------------------------------------------------------- */

const script_helper mc_lua_helpers[] = {
    { "get",    lua_mc_get    },
    { "set",    lua_mc_set    },
    { "delete", lua_mc_delete },
    { "incr",   lua_mc_incr   },
    { "decr",   lua_mc_decr   },
};

const size_t mc_lua_helpers_count =
    sizeof(mc_lua_helpers) / sizeof(mc_lua_helpers[0]);
