#ifndef SCRIPTING_LUA_HTTP1_HELPERS_H
#define SCRIPTING_LUA_HTTP1_HELPERS_H

/*
 * Registration entry point for the HTTP/1.1 Lua glue module (ADR 0001 P1-4).
 *
 * This header is deliberately thin: it declares only the registration hook and
 * pulls in NO protocol header and NO engine header. That lets engine.c call the
 * hook during init without violating Invariant 3 (engine.c must not see a
 * protocol header). The implementation, scripting/lua/http1_helpers.c, is the
 * one place permitted to include both proto/http1.h and lua.h (Invariant 4).
 */

#include "scripting/script_api.h"

/* Registers the "http" helper namespace into the given engine. Called from the
 * LuaJIT engine during init — never from a protocol implementation. */
void lua_register_http1_helpers(script_engine *engine);

#endif /* SCRIPTING_LUA_HTTP1_HELPERS_H */
