title: memcached request model — protocol-neutral request objects
status: completed
adr: 0005
adr-step: P4-1
depends: t059

## Goal

Define memcached request and response objects that fit through the existing
Request Layer and protocol vtable without adding memcached-specific core paths.

## Context

The second extension should prove the Request Layer can describe a data-store
command set without being Redis-shaped. This task creates the internal
memcached operation model before exposing Lua helpers.

## Deliverables

- memcached operation enum for `get`, `set`, `delete`, `incr`, `decr`
- request struct with key, value, flags, expiration, and delta fields as needed
- response struct with status, optional value, optional counter value, and error
  text
- conversion from request object to text codec input
- cleanup/ownership rules for request and response memory

## Guards / Acceptance

1. Unit tests cover request construction for every operation.
2. Invalid inputs are rejected before encoding.
3. Memory ownership is documented in code or tests where it is not obvious.
4. No core Request Layer API changes are required.

