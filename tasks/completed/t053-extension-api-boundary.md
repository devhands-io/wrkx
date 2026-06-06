title: Extension API/ABI boundary — stable public header and version negotiation
status: todo
adr: 0005
adr-step: P3-1
depends: t052

## Goal

Define a stable, public `include/wrkx_extension.h` that gives extension authors
everything they need to implement a protocol and register scripting helpers —
without access to any private core header. This is the **Gate C prerequisite**:
the boundary must exist as a formal interface before it can be tested by a second
protocol (memcached, Phase 4).

## Context

ADR 0005 P3-1. After Phase 2, Redis is fully working but compiled directly into
the binary. `main.c` includes `proto/redis.h` and `scripting/lua/redis_helpers.h`
by name. The extension system does not exist yet — there is no stable public API
surface, no version contract, and no documented invariant about what extensions
may and may not touch.

The risk this phase addresses: if no boundary is defined before memcached (Phase 4),
the second protocol will accidentally depend on whatever private headers Redis
happened to use, making the "extension boundary" Redis-shaped rather than general.

Currently in `src/` (private scope):
- `src/proto/proto.h` — protocol vtable (`protocol`, `connection`, `proto_status`)
- `src/scripting/script_api.h` — scripting helper types (`script_helper`,
  `script_register_helpers`, `script_engine`)

These two interfaces are the extension API surface. They need to be published to
`include/` with a version stamp and a documented "do not touch" list.

## Deliverables

### 1. `include/wrkx_extension.h`

Public header that extensions include. Must be self-contained — no other include
required for protocol or scripting-helper registration. Exposes:

```c
/* Version stamp. Extensions must check this at registration time. */
#define WRKX_EXTENSION_API_VERSION  1

/* Protocol vtable — identical to current src/proto/proto.h types.
 * The proto.h header becomes an internal implementation detail; this
 * becomes the public definition. */
typedef enum { PROTO_PENDING, PROTO_DONE, ... } proto_status;
typedef struct connection { int fd; void *proto_state; void *script_state;
                            size_t bytes; } connection;
typedef struct protocol { ... } protocol;

/* Scripting helper types — identical to the relevant subset of script_api.h. */
typedef int (*script_helper_fn)(void *engine_ctx);
typedef struct script_helper { const char *name; script_helper_fn fn; } script_helper;

/* Registration functions called by extensions at init time. */
typedef struct wrkx_extension_api {
    uint32_t version;     /* WRKX_EXTENSION_API_VERSION — checked by extension */

    /* Register a protocol vtable. Called once per protocol at startup. */
    void (*register_protocol)(const protocol *proto);

    /* Register a scripting helper namespace. ns is the Lua/JS table name
     * (e.g. "redis"). Called once per namespace after the engine is created. */
    void (*register_helpers)(const char *ns, const script_helper *helpers,
                             size_t count);
} wrkx_extension_api;

/* Entry point every extension must provide. wrkx calls this at startup,
 * passing a pointer to the API table. The extension inspects api->version
 * before using any other field. */
typedef void (*wrkx_extension_init_fn)(const wrkx_extension_api *api);
```

### 2. Internal header migration

- `src/proto/proto.h` — keep as-is for internal use, but add a comment noting
  that the stable public API is in `include/wrkx_extension.h`. No content is
  removed; the internal and public definitions must stay in sync (or the public
  header can `#include "proto/proto.h"` internally — decide which).
- `src/scripting/script_api.h` — same treatment.

Preferred approach: `include/wrkx_extension.h` is the canonical definition of
`protocol`, `connection`, `proto_status`, `script_helper`, and
`script_helper_fn`. The internal headers re-export (`#include
"../../include/wrkx_extension.h"`) rather than duplicating. This guarantees
there is exactly one definition.

### 3. Toy extension

`extensions/toy/toy.c` — minimal extension that:
- includes only `wrkx_extension.h` (no `src/` headers)
- implements `wrkx_extension_init(const wrkx_extension_api *api)` — checks
  `api->version`, calls `api->register_protocol(...)` with a trivially
  responding vtable, calls `api->register_helpers(...)` with one no-op helper
- has no functional purpose beyond proving the ABI compiles and links

### 4. Loading-failure unit tests

`tests/unit/test_extension_api.c` — tests that operate on the `wrkx_extension_api`
struct directly (no dynamic loading needed at this stage):

- `test_api_version_matches` — the version field in a constructed API struct
  equals `WRKX_EXTENSION_API_VERSION`
- `test_extension_init_called` — toy extension's `wrkx_extension_init` is called
  and sets a flag
- `test_register_protocol_invoked` — `api->register_protocol` callback is invoked
  with the toy vtable
- `test_register_helpers_invoked` — `api->register_helpers` callback is invoked
- `test_wrong_version_detected` — extension that checks `api->version != expected`
  and returns without registering; host verifies no protocol was registered
- `test_null_api_is_rejected` — extension that receives NULL api and does nothing

### 5. Documented invariant

Add `docs/extension-invariants.md` (brief, not a full ADR):

> **What extensions may not include:**
> - `src/orchestrator.h`, `src/ae.h`, `src/rate.h`, `src/transport.h`
> - Any header under `src/scripting/lua/` (engine internals)
> - Any header under `src/proto/` except via the public registration path
>
> **What extensions may include:**
> - `include/wrkx_extension.h` — this is the entire extension API surface
> - Standard C library headers
> - Their own internal headers

Add a Makefile `check-extension-headers` target that greps the toy extension's
compiled dependency list (`gcc -M`) and fails if any `src/` internal header
appears.

## Guards / Acceptance

1. `make` builds cleanly with the toy extension included.
2. All new unit tests (`test_extension_api`) pass.
3. `make check-extension-headers` passes — toy extension has zero `src/` header
   dependencies.
4. Existing Redis unit tests and E2E still pass (no regressions from any header
   restructuring).
5. `include/wrkx_extension.h` compiles in strict C99 (`-std=c99 -Wall -Wextra
   -Wpedantic`) with no warnings.

## What this does NOT do

- Does not move Redis to the extension directory (that is t055).
- Does not implement dynamic (dlopen) loading (deferred to t054's policy
  decision; static linking suffices for Gate C).
- Does not write the "how to write an extension" guide (t054).
