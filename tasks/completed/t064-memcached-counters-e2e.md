title: memcached counter E2E — incr/decr behavior
status: completed
adr: 0005
adr-step: P4-1
depends: t063

## Goal

Implement and test memcached `incr` and `decr` commands through the extension.

## Context

Counters add a different response shape from `get` and mutation status lines:
successful replies are numeric values, while missing or non-numeric keys produce
distinct server responses.

## Deliverables

- working `incr` command
- working `decr` command
- numeric response parsing and conversion
- not-found and invalid-value handling
- Lua counter workload

## Guards / Acceptance

1. E2E test covers initial set, increment, decrement, and final value.
2. E2E test covers not-found counter behavior.
3. Unit tests cover numeric overflow or range decisions if applicable.
4. No extension API changes are required.

