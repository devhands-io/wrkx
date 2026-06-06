/*
 * Redis Lua glue module — redis extension.
 * Moved from src/scripting/lua/redis_helpers.c (ADR 0005, Phase 3, P3-3).
 *
 * Exposes redis.command() and redis.pipeline() under the "redis" Lua namespace.
 * Registered into each engine via the extension API's register_helpers() call
 * in init.c — no direct script_engine dependency.
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/luajit/src/ lua headers, standard library.
 * NO src/ headers.
 */

#include "redis_lua_helpers.h"
#include "redis.h"   /* redis_make_request() */

#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#define REDIS_MAX_ARGS 64

/* -------------------------------------------------------------------------
 * redis.command(cmd, arg, ...) -> string
 * ---------------------------------------------------------------------- */

static int lua_redis_command(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    int argc = lua_gettop(L);

    if (argc == 0)
        return luaL_error(L, "redis.command: at least one argument required");
    if (argc > REDIS_MAX_ARGS)
        return luaL_error(L, "redis.command: too many arguments (max %d)",
                          REDIS_MAX_ARGS);

    const char *argv[REDIS_MAX_ARGS];
    size_t      arglens[REDIS_MAX_ARGS];

    for (int i = 0; i < argc; i++) {
        if (lua_type(L, i + 1) != LUA_TSTRING)
            return luaL_error(L, "redis.command: argument %d must be a string",
                              i + 1);
        argv[i] = lua_tolstring(L, i + 1, &arglens[i]);
    }

    size_t len = 0;
    char *buf = redis_make_request(argc, argv, arglens, &len);
    if (!buf)
        return luaL_error(L, "redis.command: failed to encode command");

    lua_pushlstring(L, buf, len);
    free(buf);
    return 1;
}

/* -------------------------------------------------------------------------
 * redis.pipeline(cmd1, cmd2, ...) -> string
 * ---------------------------------------------------------------------- */

static int lua_redis_pipeline(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    int argc = lua_gettop(L);

    if (argc == 0)
        return luaL_error(L, "redis.pipeline: at least one command required");

    size_t total = 0;
    for (int i = 1; i <= argc; i++) {
        if (lua_type(L, i) != LUA_TSTRING)
            return luaL_error(L,
                "redis.pipeline: argument %d must be a RESP string "
                "(from redis.command())", i);
        size_t len;
        lua_tolstring(L, i, &len);
        total += len;
    }

    char *buf = malloc(total);
    if (!buf)
        return luaL_error(L, "redis.pipeline: out of memory");

    size_t pos = 0;
    for (int i = 1; i <= argc; i++) {
        size_t len;
        const char *s = lua_tolstring(L, i, &len);
        memcpy(buf + pos, s, len);
        pos += len;
    }

    lua_pushlstring(L, buf, total);
    free(buf);
    return 1;
}

/* -------------------------------------------------------------------------
 * Public helper table
 * ---------------------------------------------------------------------- */

const script_helper redis_lua_helpers[] = {
    { "command",  lua_redis_command  },
    { "pipeline", lua_redis_pipeline },
};

const size_t redis_lua_helpers_count =
    sizeof(redis_lua_helpers) / sizeof(redis_lua_helpers[0]);
