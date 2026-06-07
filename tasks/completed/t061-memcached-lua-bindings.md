title: memcached Lua bindings — script helpers for text commands
status: completed
adr: 0005
adr-step: P4-1
depends: t060

## Goal

Expose memcached operations to Lua scripts through a `memcached` helper namespace.

## Context

Redis proved one extension can provide Lua helpers. memcached now checks that the
helper registration path is general enough for a second protocol with different
request shapes and response statuses.

## Deliverables

- `memcached.get(key)`
- `memcached.set(key, value, opts)`
- `memcached.delete(key)`
- `memcached.incr(key, delta)`
- `memcached.decr(key, delta)`
- Lua-to-request conversion through the memcached request model
- clear script-visible behavior for not-found and error replies

## Guards / Acceptance

1. Lua binding unit tests cover all helpers.
2. Invalid argument tests cover missing keys, wrong value types, negative deltas
   where unsupported, and malformed option tables.
3. Redis Lua helper tests still pass unchanged.
4. No Lua engine private headers are included outside allowed extension glue.

