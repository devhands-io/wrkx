title: Implement lua_configure slot per ADR 0002
status: todo
adr: 0002
adr-step: lua-configure
depends: t029

## Context

ADR 0002 Decision 3 adds a `configure` slot to `script_api`. For the LuaJIT engine
this restores the legacy `script_create(file, url, headers)` behaviour: the engine
now receives the target URL and any custom headers through `api->configure()` and
sets `wrk.scheme`, `wrk.host`, `wrk.port`, `wrk.path`, and `wrk.headers` from them.

Without this slot the Lua engine uses defaults (`scheme=http`, `host=localhost`,
`path=/`) regardless of the actual target, making URL-sensitive scripts incorrect.

`http_parser_parse_url` is already linked (via http_parser.c in the build); no new
dependency is introduced.

## Scope

- **`src/scripting/lua/engine.c`**
  - Implement:
    ```c
    static int lua_configure(script_engine *engine, const char *url,
                             const char * const *headers, size_t n_headers);
    ```
  - Parse `url` with `http_parser_parse_url` → extract `UF_SCHEMA`, `UF_HOST`,
    `UF_PORT`, `UF_PATH` offsets → push string values onto the Lua stack and
    `lua_setfield` into the `wrk` table (`wrk.scheme`, `wrk.host`, `wrk.port`,
    `wrk.path`).
  - Iterate `headers[0..n_headers-1]`; split each on the first `": "` into key
    and value; set `wrk.headers[key] = value` (create `wrk.headers` as a table
    if it doesn't exist).
  - Handle `url == NULL`, `n_headers == 0`, and missing URL fields gracefully
    (keep prior defaults).
  - Wire `lua_api.configure = lua_configure` in the static `lua_api` initialiser.

## Steps

1. Add `#include "http_parser.h"` in `engine.c` (it is a project-internal header,
   not a protocol layer header — Invariant 3 is not violated).
2. Implement `lua_configure` per above.
3. Add `.configure = lua_configure` to the `lua_api` struct initialiser.
4. Run `make test-lua-engine`; existing 9 tests must pass.
5. Add at least one new test to `tests/unit/test_lua_engine.c`:
   - create engine, call `configure("http://bench.example.com:9090/api", ...)`,
     then run a one-line Lua snippet that asserts `wrk.host`, `wrk.port`,
     `wrk.path`, and `wrk.scheme` are correct.

## Acceptance

- `make test-lua-engine` exits 0; all tests pass (9 existing + ≥1 new).
- `lua_api.configure` is non-NULL.
- After `configure("http://example.com:9090/api", NULL, 0)`:
  - `wrk.scheme == "http"`
  - `wrk.host  == "example.com"`
  - `wrk.port  == 9090`  (number, not string)
  - `wrk.path  == "/api"`
- After `configure(..., (const char*[]){"X-Foo: bar"}, 1)`:
  - `wrk.headers["X-Foo"] == "bar"`
- Invariant 3 preserved: `engine.c` does not include `proto/http1.h` or any
  other protocol implementation header.
