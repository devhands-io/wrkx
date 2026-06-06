#ifndef SCRIPTING_LUA_REDIS_HELPERS_H
#define SCRIPTING_LUA_REDIS_HELPERS_H

/*
 * Registration entry point for the Redis Lua glue module (ADR 0005, P2-2).
 *
 * Deliberately thin: declares only the registration hook with NO protocol
 * header and NO engine header (same pattern as http1_helpers.h, Invariant 4).
 * The implementation, scripting/lua/redis_helpers.c, is the ONLY translation
 * unit permitted to include both proto/redis.h and lua.h simultaneously.
 *
 * Callers: main.c (when Redis protocol is selected) and test_redis_lua.c.
 * NOT called from engine.c — Redis helper registration is protocol-aware
 * wiring that belongs at the wrkx entry point, not inside the engine.
 */

#include "scripting/script_api.h"

/* Registers the "redis" helper namespace into the given engine. */
void lua_register_redis_helpers(script_engine *engine);

#endif /* SCRIPTING_LUA_REDIS_HELPERS_H */
