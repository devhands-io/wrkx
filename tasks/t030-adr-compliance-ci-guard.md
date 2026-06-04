title: ADR-0001 compliance CI guard (no cross-layer includes)
status: todo
adr: 0001
adr-step: Compliance
depends: t025

## Context

ADR 0001's value depends on its layer boundaries staying intact over time. The ADR's
"AI-dev safety" driver and its §"Compliance" grep checks only protect the
architecture if they run automatically on every change. This task turns those checks
into a CI gate. It can land as soon as the layer directories exist (t025); it then
guards t026/t027/t028 as they implement.

## Scope

- **`scripts/adr-compliance.sh`:** runs the ADR §"Compliance" checks; exits non-zero
  (printing the offending file) on any violation:
  - no `lua.h` / `quickjs.h` under `src/proto/`
  - no `#include .*proto/` in `src/scripting/*/engine.c` (glue modules excluded)
  - no `lua.h` / `quickjs.h` in `src/orchestrator.c`
  - (extend with the other Compliance bullets as those files appear, e.g. a new
    protocol must arrive as a new `proto/*.c`, not by editing `orchestrator.c`)
- **CI wiring:** add a step invoking the script to the GitHub Actions workflow from
  t017, so every PR is checked.

## Steps

- Write `scripts/adr-compliance.sh` (portable `grep`; guard against the "no match =
  grep exit 1" trap so a clean tree passes).
- Make it skippable-clean when a layer dir doesn't exist yet (Phase 1 is incremental).
- Add a `make adr-check` convenience target invoking the script.
- Add a CI job/step running `make adr-check` (or the script directly).

## Acceptance

- Script exits 0 on the current clean tree.
- Script exits non-zero on an injected violation (e.g. a temporary `#include <lua.h>`
  in a `proto/` file), printing which file/rule failed.
- CI runs the guard on every PR and fails the build on violation.
