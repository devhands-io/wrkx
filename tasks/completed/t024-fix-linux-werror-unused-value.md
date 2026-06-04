title: Fix Linux `-Werror=unused-value` in test_script.c (`luaL_dostring`)
status: completed
depends: t23

## Problem

GCC on Linux emits `-Werror=unused-value` for every bare `luaL_dostring()` call
in `tests/unit/test_script.c`. The macro in LuaJIT's `lauxlib.h` expands to:

```c
(luaL_loadstring(L, s) || lua_pcall(L, 0, LUA_MULTRET, 0))
```

The `||` expression produces a value (0 or 1) that is silently discarded when
the macro is used as a statement. GCC flags this; Clang does not.

Affected call sites (4):

| Line | Call |
|------|------|
| 128  | `luaL_dostring(L, "function request() return 'PING' end");` |
| 156  | `luaL_dostring(L, "function request() return wrk.format() end");` |
| 175  | `luaL_dostring(L, "function response(status, headers, body) end");` |
| 194  | `luaL_dostring(L, "function done(summary, latency, requests) end");` |

(Line 41 in `init_wrk()` is already error-checked via an `if` — not affected.)

## Fix

Wrap all four bare `luaL_dostring()` call sites with `(void)` to explicitly
discard the return value and suppress the warning on both GCC and Clang:

```c
// before
luaL_dostring(L, "function request() return 'PING' end");

// after
(void)luaL_dostring(L, "function request() return 'PING' end");
```

**File:** `tests/unit/test_script.c` — four changes, no other files touched.

## Verification

```bash
make clean && make && make test
```

- Zero warnings, zero errors on macOS (Clang)
- Linux CI job (`ubuntu-latest`) must go green — specifically `obj/test_script`
  must compile and all 42 tests must pass

## Commit

Single commit:

```
fix: suppress luaL_dostring unused-value warning on GCC (Linux)

Cast four bare luaL_dostring() calls to (void) in test_script.c.
GCC's -Werror=unused-value fires on the macro's || expression result;
Clang is silent. The (void) cast is the idiomatic cross-compiler fix.
```
