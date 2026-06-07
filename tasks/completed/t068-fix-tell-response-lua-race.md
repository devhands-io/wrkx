title: fix segfault — tell_response calls LuaJIT from all threads concurrently
status: completed
adr: 0001

## Problem

Running wrkx with a high thread + connection count crashes with SIGSEGV inside
LuaJIT's `lj_str_new` during a live load test:

    ./wrkx -c218 -t27 -d20 -L -R50000 http://localhost
    ...
    zsh: segmentation fault  ./wrkx ...

Crash report (wrkx-2026-06-07-155708.ips) shows:

    faulting thread stack:
      lj_str_new          (LuaJIT)
      record_response     (orchestrator.c)
      socket_readable     (orchestrator.c)
      ...

    EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0x000000000000000f

## Root cause

`tell_response` calls `api->response(t->engine, ...)` on every completed
response, but `t->engine` is the same shared `lua_State *` for all threads
(set once in `orchestrator_create` as `t->engine = engine`).  LuaJIT is not
thread-safe.  With N threads all completing responses simultaneously, every
thread enters LuaJIT concurrently on the single state — a data race that
corrupts internal LuaJIT string-table pointers and crashes.

The race becomes reliably reproducible at high thread + rate combinations
because the probability of concurrent `tell_response` calls grows with both.

## Why ask_request was safe

`ask_request` already guards the LuaJIT call with `t->dynamic`:

    if (t->dynamic && api && api->request && t->engine) { ... }

When `t->dynamic = false` (no per-request Lua script), it falls through to the
pre-generated static request buffer — no LuaJIT access.

`tell_response` was missing the identical guard, so it always called into Lua
regardless of whether a response handler existed or the request was static.

## Fix

Add `t->dynamic &&` to the condition in `tell_response`, matching `ask_request`:

    - if (api && api->response && t->engine)
    + if (t->dynamic && api && api->response && t->engine)

One-line change in `src/orchestrator.c`.

## Verification

`make test` passes.  The crashing invocation completes cleanly:

    ./wrkx -c218 -t27 -d20 -L -R50000 http://localhost
