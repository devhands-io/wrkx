title: memcached CI service — run extension E2E in automation
status: todo
adr: 0005
adr-step: P4-2
depends: t065

## Goal

Add memcached service support to CI and run the memcached E2E suite automatically.

## Context

ADR 0005 P4-2 requires E2E coverage against a real memcached service. Local tests
are not enough for Gate C' because extension build and service setup must remain
repeatable in automation.

## Deliverables

- CI memcached service container or equivalent setup
- memcached E2E job or matrix entry
- documented local command for running memcached E2E
- build configuration that includes the memcached extension in CI

## Guards / Acceptance

1. CI runs memcached E2E on Linux.
2. macOS behavior is either covered or explicitly documented as skipped because
   service containers are unavailable.
3. Redis and HTTP CI coverage remains unchanged.
4. CI failure output makes service-start failures distinguishable from protocol
   failures.

