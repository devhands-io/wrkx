#ifndef PG_LUA_HELPERS_H
#define PG_LUA_HELPERS_H

/*
 * pg_lua_helpers.h — PostgreSQL Lua helper table.
 *
 * Internal to extensions/postgres/.  Exposed so init.c can pass the array
 * to api->register_helpers() and test_pg_lua.c can bind helpers directly.
 */

#include "wrkx_extension.h"   /* script_helper */

extern const script_helper postgres_lua_helpers[];
extern const size_t        postgres_lua_helpers_count;

#endif /* PG_LUA_HELPERS_H */
