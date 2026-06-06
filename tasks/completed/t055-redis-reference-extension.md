title: Convert Redis to reference extension — Gate C confirmation
status: todo
adr: 0005
adr-step: P3-3
depends: t054

## Goal

Move Redis from `src/proto/redis.*` and `src/scripting/lua/redis_helpers.*` into
`extensions/redis/`, built and registered through the extension mechanism from t054.
After this move, `main.c` must not include any Redis-specific header. This is the
**Gate C test**: if Redis can be loaded as a real extension with no private-header
access, the extension boundary is proven to exist.

## Context

ADR 0005 P3-3. After t053 (API/ABI boundary) and t054 (build support), the
extension infrastructure exists. t055 is the validation test: does the real Redis
implementation actually fit through the extension API without cheating?

Redis is the ideal candidate for this move because:
- It was designed alongside the extension boundary (no pre-existing private-header
  coupling was intended)
- Its full E2E test suite already exists and can detect regressions
- Gate C is defined precisely as "Redis is a real extension with no private-header
  access"

A failure here (e.g. Redis needing `transport.h` or `orchestrator.h`) is Gate C
failing — the extension boundary must be revised before Phase 4.

## Current state (before this task)

Files to move:
- `src/proto/redis.c` → `extensions/redis/redis.c`
- `src/proto/redis.h` → `extensions/redis/redis.h` (or dropped; see below)
- `src/proto/resp.c` → `extensions/redis/resp.c`
- `src/proto/resp.h` → `extensions/redis/resp.h`
- `src/scripting/lua/redis_helpers.c` → `extensions/redis/redis_lua_helpers.c`
- `src/scripting/lua/redis_helpers.h` → `extensions/redis/redis_lua_helpers.h`

`main.c` currently includes `proto/redis.h` and `scripting/lua/redis_helpers.h`
and calls `redis_configure(...)`, `lua_register_redis_helpers(...)` and
`redis_protocol()` directly. After this move, all three call sites must be removed
and replaced by the extension registration path from t054.

## Deliverables

### 1. Move Redis source files to `extensions/redis/`

```
extensions/redis/
    redis.c              # vtable implementation (was src/proto/redis.c)
    redis.h              # internal Redis header (private to the extension)
    resp.c               # RESP codec (was src/proto/resp.c)
    resp.h               # RESP codec header (private to the extension)
    redis_lua_helpers.c  # Lua glue (was src/scripting/lua/redis_helpers.c)
    redis_lua_helpers.h  # Lua glue header (private to the extension)
    Makefile.ext         # build fragment
    init.c               # extension entry point (new — see below)
```

`redis.h` and `resp.h` are now internal to the extension. Nothing outside
`extensions/redis/` may include them. The public registration is done through
`wrkx_extension.h` alone.

### 2. Extension entry point: `extensions/redis/init.c`

```c
#include "wrkx_extension.h"
#include "redis.h"
#include "redis_lua_helpers.h"

void wrkx_extension_init_redis(const wrkx_extension_api *api) {
    if (api->version != WRKX_EXTENSION_API_VERSION) return;
    api->register_protocol(redis_protocol());
    /* Scripting helpers are registered per-engine in configure, not here.
     * See note on helper registration timing below. */
}
```

**Note on helper registration timing.** The current `lua_register_redis_helpers(eng)`
is called after engine creation in `main.c`. The extension API needs a way to hook
into per-engine setup. Two options:

- **Option A:** Add `void (*on_engine_created)(script_engine *)` to
  `wrkx_extension_api` — called for each new engine. Extension uses it to call
  `api->register_helpers(...)`. Preferred: keeps the extension fully self-contained.
- **Option B:** The extension registers helpers eagerly with a deferred function
  pointer that `main.c` calls at the right time. Less clean.

Choose Option A and add `on_engine_created` to `wrkx_extension_api` as part of
this task (version bump to 2 if the struct layout changes, or keep v1 and add
it as a nullable function pointer).

### 3. Remove Redis from `src/`

- Delete `src/proto/redis.c`, `src/proto/redis.h`, `src/proto/resp.c`,
  `src/proto/resp.h`
- Delete `src/scripting/lua/redis_helpers.c`, `src/scripting/lua/redis_helpers.h`
- Remove Redis from `Makefile` source lists and dependency rules (the extension
  build fragment takes over)
- Remove `#include "proto/redis.h"` and `#include "scripting/lua/redis_helpers.h"`
  from `main.c`
- Remove `redis_configure(...)`, `lua_register_redis_helpers(...)`, and
  `redis_protocol()` call sites from `main.c`

After the move, `src/proto/` contains only: `http1.c`, `http1.h`, `proto.h`.
`src/scripting/lua/` contains only the Lua engine and its HTTP helpers.

### 4. Gate C header check

`make EXTENSIONS=redis check-extension-headers` must pass: no file in
`extensions/redis/` may include any header from `src/`.

The Lua engine headers (`lua.h`, `lualib.h`, `lauxlib.h`) come from
`deps/luajit/` — those are not `src/` headers and are permitted in the glue
module.

### 5. Transport access removal (if needed)

Currently `redis.c` includes `transport.h` (from `src/transport.h`) to call
`transport_connect`, `transport_read`, `transport_write`, `transport_close`.
Transport is a private core module — an extension must not include it directly.

Resolution: expose a transport-like facility through `wrkx_extension_api`, OR
move transport to `include/` as a second stable interface, OR provide the
transport operations as callbacks in the API struct.

Preferred: add transport callbacks to `wrkx_extension_api`:

```c
typedef struct wrkx_transport wrkx_transport;   /* opaque */

typedef struct wrkx_extension_api {
    uint32_t version;
    void (*register_protocol)(const protocol *);
    void (*register_helpers)(const char *ns, const script_helper *, size_t);
    void (*on_engine_created)(script_engine *);   /* nullable; called per engine */

    /* Transport primitives — extensions use these instead of transport.h */
    int  (*transport_connect)(wrkx_transport *, struct addrinfo *, SSL_CTX *,
                              const char *host, int *fd_out);
    int  (*transport_read)(wrkx_transport *, char *buf, size_t cap, size_t *n);
    int  (*transport_write)(wrkx_transport *, const char *buf, size_t len, size_t *n);
    void (*transport_close)(wrkx_transport *);
} wrkx_extension_api;
```

Alternatively: move `transport.h` to `include/wrkx_transport.h` and publish it as
a stable second interface — simpler if transport is already stable. Decide which
during implementation; document the choice here.

### 6. `Makefile.ext` for Redis extension

```makefile
EXT_NAME  = redis
EXT_SRCS  = extensions/redis/redis.c \
            extensions/redis/resp.c   \
            extensions/redis/redis_lua_helpers.c \
            extensions/redis/init.c
EXT_CFLAGS = -Iextensions/redis -Ideps/luajit/src
```

### 7. Build Redis as a default extension

Update the top-level `Makefile` so that `make` (no flags) still builds a binary
that includes Redis. Two options:
- `EXTENSIONS ?= redis` (default to Redis in Makefile)
- Explicit `wrkx: EXTENSIONS += redis` target

The important invariant: `make EXTENSIONS=` (empty) produces a binary with only
HTTP/1.1, and `make` (or `make EXTENSIONS=redis`) produces a binary with both.
Both must build and test cleanly.

## Guards / Acceptance

1. **`make EXTENSIONS=redis`** builds cleanly — no errors, no warnings.
2. **`make EXTENSIONS=redis check-extension-headers`** passes — extensions/redis/
   contains zero `src/` includes.
3. **`make EXTENSIONS=redis test-redis`** — all 24 Redis unit tests pass.
4. **`make EXTENSIONS=redis test-redis-lua`** — all 17 Redis Lua unit tests pass.
5. **`bash tests/e2e/redis_basic.sh`** — passes against the dummy RESP server.
6. **`bash tests/e2e/redis_pipeline.sh`** — passes (pipelining unaffected).
7. **`make EXTENSIONS=redis gate-a-check`** — still PASS.
8. **`make EXTENSIONS= test`** (no Redis) — full suite passes; binary works for
   HTTP/1.1 workloads; attempting `redis://` URL exits cleanly with an error
   ("redis protocol not available; rebuild with EXTENSIONS=redis").
9. **Gate C:** `git diff HEAD -- src/proto/redis.c src/proto/resp.c` shows those
   files deleted; `extensions/redis/` contains their replacements; `main.c` contains
   no Redis-specific include or call site.

## Gate C definition (from ADR 0005)

> **Gate C — extension independence**
> Passed by: Redis loaded as a real extension (P3-3).
> Failure condition: extensions need private core headers or special-case build wiring.

Gate C passes when item 2 above (`check-extension-headers`) is green AND the E2E
tests (items 5–6) pass with Redis loaded via the extension mechanism.

## What this does NOT do

- Does not implement memcached (Phase 4).
- Does not add HTTP/1.1 as an extension (it remains compiled into the core; the
  extension system is for optional/third-party protocols).
- Does not implement dynamic loading.
