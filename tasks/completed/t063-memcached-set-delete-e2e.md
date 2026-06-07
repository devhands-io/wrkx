title: memcached set/delete E2E — mutation command coverage
status: completed
adr: 0005
adr-step: P4-1
depends: t062

## Goal

Complete the basic mutation commands for the memcached text protocol: `set` and
`delete`.

## Context

After `get` proves the connection and read path, `set` and `delete` prove request
bodies, storage status handling, and multi-step workload behavior.

## Deliverables

- working `set` command over the protocol vtable
- working `delete` command over the protocol vtable
- Lua workload that performs `set` -> `get` -> `delete` -> `get`
- response mapping for stored, not stored, deleted, and not found

## Guards / Acceptance

1. E2E test verifies the full set/get/delete sequence.
2. Parser unit tests cover body length and CRLF handling for values.
3. Error count distinguishes protocol/server failures from expected not-found
   results.
4. Existing Redis E2E remains green.

