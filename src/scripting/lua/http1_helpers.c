/* src/scripting/lua/http1_helpers.c
 *
 * HTTP/1.1 Lua glue module (ADR 0001, Phase 1, P1-4).
 *
 * GLUE MODULE EXEMPLAR. This is the ONLY translation unit permitted to include
 * both a protocol header (proto/http1.h) and an engine header (lua.h) — see
 * Invariant 4. It knows exactly two things: LuaJIT's native call convention,
 * and the protocol's public C API. Neither proto/http1.c nor the engine
 * (scripting/lua/engine.c) knows about the other; this file bridges them.
 *
 * The helpers below are intentionally minimal — they demonstrate the pattern
 * (marshal Lua args, call the protocol C API, marshal the result back) rather
 * than expose the full protocol surface.
 */

#include <lua.h>                  /* engine call convention (Invariant 4)      */
#include <lauxlib.h>

#include "scripting/script_api.h"
#include "scripting/lua/engine.h" /* lua_engine_state() accessor               */
#include "scripting/lua/http1_helpers.h"
#include "proto/http1.h"          /* protocol C API (Invariant 4)              */

/*
 * http.name() -> string
 * Returns the HTTP/1.1 protocol vtable's name, reached through the protocol's
 * public C API (proto/http1.h). Illustrates a glue helper calling into the
 * Protocol Engine without the engine itself ever seeing a protocol header.
 */
static int lua_http1_name(void *engine_ctx) {
    lua_State *L = (lua_State *) engine_ctx;
    protocol *p = http1_protocol();
    lua_pushstring(L, p->name);
    return 1;
}

static const script_helper http1_helpers[] = {
    { "name", lua_http1_name },
    { NULL,   NULL           },
};

void lua_register_http1_helpers(script_engine *engine) {
    size_t count = (sizeof(http1_helpers) / sizeof(http1_helpers[0])) - 1;
    lua_register_helpers(engine, "http", http1_helpers, count);
}
