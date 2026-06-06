title: Redis Lua scripting bindings — redis.command() via Request Layer
status: todo
adr: 0005
adr-step: P2-2
depends: t049

## Goal

Expose Redis commands to Lua workload scripts through the Request Layer
(`script_api.h`), without Lua-specific logic bleeding into the protocol layer.
The glue module (`src/scripting/lua/redis_helpers.c`) is the only place both
headers coexist — enforcing ADR 0001 Invariant 4.

## Context

ADR 0005 P2-2. With the Redis protocol vtable in place (t049), this task wires
the scripting side: a Lua helper module that lets scripts call
`redis.command("SET", "key", "value")` and receive the reply as a Lua value.
Request objects are constructed through the language-neutral `script_api.h`
request-construction API (not by directly touching RESP buffers from Lua).

## Deliverables

- `src/scripting/lua/redis_helpers.c` — glue module:
  - `redis.command(cmd, arg1, ...)` → issues one Redis command, returns reply
  - `redis.pipeline(...)` — stubbed (returns error or no-op) with a clear
    TODO comment pointing to t052; do not implement now
- Registration via `script_register_helpers()` at startup when Redis is the
  selected protocol
- Request Layer object construction: `redis_helpers.c` calls script_api
  request-construction functions, not RESP encode functions directly
- Reply conversion: status, error, integer, bulk string, array → Lua types

## Guards / Acceptance

1. **Lua binding unit tests** — `tests/unit/test_redis_lua.c` (or `.lua`):
   - `redis.command("PING")` → `"PONG"`
   - `redis.command("SET", "k", "v")` → `"+OK"`
   - `redis.command("GET", "k")` → `"v"` (bulk string)
   - `redis.command("INCR", "counter")` → integer reply
   - `redis.command("BADCMD")` → error reply surfaced as Lua error (not crash)
2. **Invalid-argument tests:**
   - zero arguments → Lua error
   - non-string argument where string expected → Lua error
3. **Script-to-protocol object test:**
   - confirm `redis_helpers.c` does not `#include` any RESP header directly;
     protocol construction goes through `script_api.h` request objects only
   - `grep -r 'resp\.h' src/scripting/` must be empty
4. `make test` green.

## Note on pipeline stub

`redis.pipeline(...)` must exist as a function (so scripts can be written
anticipating it) but must return a clear runtime error: `"pipeline not yet
implemented — see t052"`. This prevents scripts silently succeeding with
incomplete behaviour.
