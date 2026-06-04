#ifndef SCRIPTING_LUA_ENGINE_H
#define SCRIPTING_LUA_ENGINE_H

/*
 * LuaJIT Request-Layer engine (ADR 0001, Phase 1, P1-4).
 *
 * Implements the frozen script_api vtable from scripting/script_api.h for
 * LuaJIT, preserving today's wrk.* Lua API and the init/request/response/done
 * hook semantics. The orchestrator obtains the engine purely through the
 * vtable returned here; it never sees a lua_State.
 *
 * Invariant 3: this engine must not #include any protocol header. Protocol
 * helpers reach the engine only through glue modules
 * (scripting/lua/<proto>_helpers.c), which are the sole place a protocol header
 * and an engine header coexist (Invariant 4).
 */

#include "scripting/script_api.h"

/* Returns the singleton LuaJIT script_api vtable. */
script_api *lua_script_api(void);

/*
 * Exposed for glue modules only. A glue module is handed the script_engine *
 * during engine init and uses this accessor to reach the underlying lua_State
 * so it can register its native call-convention closures. Declared with void *
 * return so this header pulls in no engine (lua.h) header; the glue module
 * casts it back to lua_State *.
 */
void *lua_engine_state(script_engine *engine);

#endif /* SCRIPTING_LUA_ENGINE_H */
