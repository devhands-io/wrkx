title: memcached extension skeleton — register a second protocol extension
status: completed
adr: 0005
adr-step: P4-1
depends: t055

## Goal

Create the minimal memcached extension shell without implementing real protocol
behavior yet. This starts Phase 4 by proving a second extension can be built and
registered through the existing extension mechanism.

## Context

ADR 0005 Phase 4 tests Gate C': the extension API must not be Redis-shaped.
Before adding memcached protocol details, the project needs a small compile-time
proof that a new extension can exist beside Redis without touching core engine or
orchestrator code.

## Deliverables

- `extensions/memcached/` directory
- `extensions/memcached/Makefile.ext`
- extension init entry point
- stub memcached protocol vtable
- default-disabled or explicitly selectable build integration
- header dependency check coverage for the memcached extension

## Guards / Acceptance

1. `make EXTENSIONS=memcached` builds cleanly.
2. `make EXTENSIONS=redis memcached` or the project-supported equivalent builds
   both extensions together.
3. `make check-extension-headers` confirms memcached includes no private `src/`
   headers.
4. No changes to scheduler, orchestrator, or rate-control semantics.
