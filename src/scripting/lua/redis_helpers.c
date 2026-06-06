/* src/scripting/lua/redis_helpers.c
 *
 * Redis Lua glue module (ADR 0005, Phase 2, P2-2).
 *
 * GLUE MODULE — the ONLY translation unit that may include both a Redis
 * protocol header (proto/redis.h) and engine headers (lua.h) simultaneously.
 * See Invariant 4. Neither redis.c nor engine.c includes the other; this file
 * bridges them.
 *
 * Exposes two helpers under the "redis" Lua namespace:
 *
 *   redis.command(cmd, arg, ...)
 *     Formats a Redis command as RESP request bytes and returns them as a Lua
 *     string. Intended for use inside the request() hook:
 *
 *       function request()
 *           return redis.command("SET", "k", "v")
 *       end
 *
 *     The RESP bytes are forwarded by the orchestrator through the redis
 *     protocol vtable (redis.c) to the server. proto/resp.h is NOT included
 *     here — encoding is delegated to redis_make_request() (proto/redis.h).
 *
 *   redis.pipeline(...)
 *     Stub. Raises a Lua error with a "not yet implemented" message.
 *     Pipelining is Gate B / t052.
 *
 * Invariant 2: no scripting header is included in proto/redis.h (verified).
 * Invariant 3: engine.c does not include this file (verified).
 */

#include <stdlib.h>
#include <string.h>

#include <lua.h>       /* engine call convention (Invariant 4) */
#include <lauxlib.h>

#include "scripting/script_api.h"
#include "scripting/lua/engine.h"        /* lua_engine_state() */
#include "scripting/lua/redis_helpers.h"
#include "proto/redis.h"                 /* redis_make_request() — NOT resp.h */

#define REDIS_MAX_ARGS 64

/*
 * redis.command(cmd, arg, ...) -> string
 *
 * Marshals one or more Lua string arguments into a RESP bulk-string array
 * and returns the raw bytes as a Lua string. The caller (user script's
 * request() hook) returns that string to the orchestrator, which forwards it
 * through redis_write() to the server.
 *
 * Error conditions that raise a Lua error (not a crash):
 *   - zero arguments
 *   - more than REDIS_MAX_ARGS arguments
 *   - a non-string argument
 *   - allocation / encoding failure in redis_make_request()
 */
static int lua_redis_command(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    int argc = lua_gettop(L);

    if (argc == 0)
        return luaL_error(L, "redis.command: at least one argument required");
    if (argc > REDIS_MAX_ARGS)
        return luaL_error(L, "redis.command: too many arguments (max %d)", REDIS_MAX_ARGS);

    const char *argv[REDIS_MAX_ARGS];
    size_t      arglens[REDIS_MAX_ARGS];

    for (int i = 0; i < argc; i++) {
        if (lua_type(L, i + 1) != LUA_TSTRING)
            return luaL_error(L, "redis.command: argument %d must be a string", i + 1);
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

/*
 * redis.pipeline(cmd1, cmd2, ...) -> string
 *
 * Concatenates pre-encoded RESP strings (each produced by redis.command())
 * into one buffer. The resulting string, when returned from request(), causes
 * the Redis protocol vtable to send all commands in one write and accumulate
 * all replies before returning PROTO_DONE — the pipeline depth is auto-detected
 * from the number of RESP top-level values in the buffer.
 *
 * Example:
 *   function request()
 *       local key = "k:" .. math.random(1, 1000)
 *       return redis.pipeline(
 *           redis.command("SET", key, "val"),
 *           redis.command("GET", key)
 *       )
 *   end
 */
static int lua_redis_pipeline(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    int argc = lua_gettop(L);

    if (argc == 0)
        return luaL_error(L, "redis.pipeline: at least one command required");

    /* First pass: validate args and compute total length. */
    size_t total = 0;
    for (int i = 1; i <= argc; i++) {
        if (lua_type(L, i) != LUA_TSTRING)
            return luaL_error(L, "redis.pipeline: argument %d must be a "
                              "RESP string (from redis.command())", i);
        size_t len;
        lua_tolstring(L, i, &len);
        total += len;
    }

    char *buf = malloc(total);
    if (!buf)
        return luaL_error(L, "redis.pipeline: out of memory");

    /* Second pass: concatenate. */
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

static const script_helper redis_helpers[] = {
    { "command",  lua_redis_command  },
    { "pipeline", lua_redis_pipeline },
    { NULL,       NULL               },
};

void lua_register_redis_helpers(script_engine *engine) {
    size_t count = (sizeof(redis_helpers) / sizeof(redis_helpers[0])) - 1;
    script_register_helpers(engine, "redis", redis_helpers, count);
}
