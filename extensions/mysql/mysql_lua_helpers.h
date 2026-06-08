#ifndef MYSQL_LUA_HELPERS_H
#define MYSQL_LUA_HELPERS_H

/*
 * mysql_lua_helpers.h — MySQL Lua helper table.
 *
 * Internal to extensions/mysql/.  Exposed so init.c can pass the array
 * to api->register_helpers() and test_mysql_lua.c can bind helpers directly.
 *
 * ADR 0005, Phase 6 (P6-4).
 */

#include "wrkx_extension.h"   /* script_helper */

extern const script_helper mysql_lua_helpers[];
extern const size_t        mysql_lua_helpers_count;

#endif /* MYSQL_LUA_HELPERS_H */
