# wrkx — session context for Claude Code

## Task workflow

### Locations
- **Active tasks:** `tasks/t<NNN>-<slug>.md`
- **Completed tasks:** `tasks/completed/t<NNN>-<slug>.md`

### Task file format
```
title: <short description>
status: pending | completed
adr: <ADR number>
adr-step: <phase step, e.g. P5-1>
depends: t<NNN>

## Why / Goal
## Deliverables
## Guards
## Core engine touch
```

### Creating a task
- Write to `tasks/t<NNN>-<slug>.md` with `status: pending`
- Keep deliverables granular and self-contained
- List explicit guards (the exact checks that must pass)
- State "core engine touch" — what is and isn't allowed to change

### Executing a task
1. Read the task file first, then read every file it touches
2. Implement deliverables in dependency order
3. Run guards: `make test`, `make test-asan`; verify frozen-file diffs are empty (`include/wrkx_extension.h`, etc.)
4. **Before committing:** `cp` the task file to `tasks/completed/`, update `status: completed`, then `rm` the original and `git rm` it
5. Commit implementation + moved task file together

### Commit convention
- Every commit message ends with:
  ```
  Co-Authored-By: Claude {model} <noreply@anthropic.com>
  ```
  where `{model}` is the active model name (e.g. `Sonnet 4.5`, `Opus 4`).
- If the task file deletion is accidentally left unstaged, follow up with a separate `chore(tasks): remove tNNN from active tasks` commit.

## Architecture overview

Three-layer design (ADR 0001):
- **Protocol Engine** (`src/proto/`) — knows the wire format; no scripting
- **Orchestrator** (`src/orchestrator.c`) — thread pool, event loop, rate control; no protocol or scripting specifics
- **Request Layer** (`src/scripting/`) — scripting engines (LuaJIT, QuickJS); no protocol headers (Invariant 3)

Extensions live in `extensions/<name>/` and register via `include/wrkx_extension.h` (frozen ABI).

## Current phase

**ADR 0005 Phase 5 (Gate D):** prove the Request Layer is not Lua-shaped by running QuickJS alongside LuaJIT against the Redis extension with identical wire output.

Active task chain: t072 → t073 → t074 → t075 → t076
