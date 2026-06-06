#ifndef REDIS_LUA_HELPERS_H
#define REDIS_LUA_HELPERS_H

/*
 * Redis Lua helper table — internal to the redis extension.
 * Exposed so that init.c can pass the array to api->register_helpers()
 * and test_redis_lua.c can bind helpers directly for unit testing.
 */

#include "wrkx_extension.h"   /* script_helper type */

extern const script_helper redis_lua_helpers[];
extern const size_t        redis_lua_helpers_count;

#endif /* REDIS_LUA_HELPERS_H */
