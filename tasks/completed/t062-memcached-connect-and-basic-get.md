title: memcached basic GET E2E — connect, send, receive
status: completed
adr: 0005
adr-step: P4-1
depends: t061

## Goal

Wire the memcached protocol vtable to real network I/O and prove a minimal `get`
workload works against a live memcached server.

## Context

This is the first real vertical slice for memcached. Keep the command scope to
`get` so connection lifecycle, request dispatch, response parsing, and metrics can
be validated before adding mutations.

## Deliverables

- connect lifecycle for memcached
- one-command send/receive path for `get`
- response handling for hit and miss
- minimal Lua E2E workload
- local E2E script or test harness support for a real memcached instance

## Guards / Acceptance

1. E2E test confirms `get` hit and miss behavior against real memcached.
2. Latency, throughput, and error reporting use existing machinery unchanged.
3. Connection teardown is clean under normal completion.
4. No core engine or orchestrator changes are required.

