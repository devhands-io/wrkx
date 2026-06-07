title: memcached connection reuse and errors — robustness pass
status: todo
adr: 0005
adr-step: P4-1
depends: t064

## Goal

Harden memcached connection reuse, reconnect behavior, timeout/error reporting,
and malformed-response handling.

## Context

The Phase 4 claim is stronger than "commands work once." memcached must behave
like a real protocol extension under repeated requests and ordinary failure modes
without relying on Redis-specific assumptions.

## Deliverables

- connection reuse across multiple requests
- reconnect behavior after server-side close
- timeout and short-read handling
- malformed response detection
- clear mapping from memcached errors to wrkx error reporting

## Guards / Acceptance

1. E2E test exercises many requests over reused connections.
2. Failure-mode tests cover server close, malformed replies, and timeout paths.
3. No unbounded buffering under slow or partial responses.
4. Existing HTTP and Redis E2E tests still pass.

