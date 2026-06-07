title: memcached example and Gate C' audit — confirm extension API independence
status: completed
adr: 0005
adr-step: P4-2
depends: t066

## Goal

Finish Phase 4 by adding a memcached example workload, a throughput/latency smoke
check, and the Gate C' audit.

## Context

Gate C' is confirmed only if memcached works as a second extension without
requiring changes to the extension API. If memcached forced API changes, the plan
must stop here and ADR 0005 should be revised before moving to Phase 5.

## Deliverables

- example Lua workload under the project script/example convention
- throughput and latency smoke test against real memcached
- final audit of extension API changes since t055
- short note documenting whether Gate C' passed

## Guards / Acceptance

1. Example workload runs successfully against real memcached.
2. Throughput smoke test reaches the configured rate target within existing
   tolerance conventions.
3. `git diff` or equivalent review confirms no scheduler/orchestrator changes
   were needed for memcached.
4. Gate C' is documented as passed only if no extension API change was required.
5. If an extension API change was required, document it here and open/update an
   ADR before beginning Phase 5.

## Gate C' Audit (2026-06-07)

**PASSED.**

`git diff 98dcf9c HEAD -- include/wrkx_extension.h` → empty.
`git diff 98dcf9c HEAD -- src/orchestrator.c src/orchestrator.h` → empty.

The only post-Gate-C main.c touch (t058, commit cb8e867) was a 6-line fix to
carry the correct protocol pointer per registered schema when multiple extensions
are loaded simultaneously — not an API change, a host-side correctness fix.

`wrkx_extension.h` (WRKX_EXTENSION_API_VERSION 2) is unchanged since t055.
No new fields, no new callbacks, no new includes required by the memcached
extension. memcached's `init.c` calls the same `register_protocol`,
`register_helpers`, and `register_schema` hooks that Redis uses.

The Phase 4 memcached extension (t058–t067) is complete proof that the
extension API generalises beyond Redis. Phase 5 may proceed.

## Smoke test result

    ./wrkx -t1 -c10 -d5s -R100 -s scripts/memcached_example.lua memcached://127.0.0.1:11211
    Requests/sec: 100.18   (target 100, tolerance ±5% → pass)

