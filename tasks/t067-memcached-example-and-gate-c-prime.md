title: memcached example and Gate C' audit — confirm extension API independence
status: todo
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

