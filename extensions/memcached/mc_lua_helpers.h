#ifndef MC_LUA_HELPERS_H
#define MC_LUA_HELPERS_H

/*
 * mc_lua_helpers.h — memcached Lua helper table.
 *
 * Internal to extensions/memcached/.  Exposed so init.c can pass the array
 * to api->register_helpers() and test_mc_lua.c can bind helpers directly.
 */

#include "wrkx_extension.h"   /* script_helper */

extern const script_helper mc_lua_helpers[];
extern const size_t        mc_lua_helpers_count;

#endif /* MC_LUA_HELPERS_H */
