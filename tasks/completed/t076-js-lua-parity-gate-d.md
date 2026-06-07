title: JS/Lua Redis workload parity + Gate D confirmation
status: pending
adr: 0005
adr-step: P5-4
depends: t075

## Goal

Prove Gate D: the same Redis workload, expressed in Lua and in JavaScript,
produces the same wire output while the protocol and orchestrator layers stay
unchanged across the QuickJS work.

## Gate D criterion (restated from ADR 0005)

> The Request Layer abstraction is not Lua-shaped — a second scripting engine
> implements it without changes to the protocol or orchestrator layers.

Concretely:
1. `scripts/redis_get_set.lua` and `scripts/redis_get_set.js` produce the same
   multiset of Redis wire commands for the same logical run.
2. The **protocol layer** and **orchestrator scheduling/event-loop** are unchanged
   across the QuickJS-adding tasks (P5-2 → P5-4).
3. Both engines build and run through the identical orchestrator startup path.

## Baseline, not commit-grep

The earlier draft greped commit messages and diffed a non-existent
`src/scheduler.c`.  Instead, record an explicit git baseline tag at the **start of
P5-2** (i.e. immediately after t070 lands — the point where the Request Layer
contract is finalized and before any QuickJS engine code that must not perturb the
core).  Gate D diffs explicit paths against that tag.

P5-1 legitimately changes `src/scripting/script_api.h` and the Lua engine — those
are **expected** and are not part of the Gate D frozen set.  The frozen set is the
protocol + orchestrator core only.

### Create the baseline (one-time, when t070 is committed)

```sh
git tag p5-baseline   # at the commit that completes t070
```

Record the resolved hash in this task file when executed.

## Frozen path set (must not change P5-2 → P5-4)

```
# Orchestrator core (scheduling, event loop, connection/rate logic):
src/orchestrator.c   src/orchestrator.h
src/ae.c  src/ae.h  src/ae_epoll.c  src/ae_kqueue.c  src/ae_select.c  src/ae_evport.c
src/rate.c  src/rate.h
src/net.c  src/net.h  src/transport.c  src/transport.h

# Extension-facing + protocol contracts:
include/wrkx_extension.h   include/wrkx_transport.h

# Redis protocol implementation:
extensions/redis/         (protocol + parser; helper-TABLE extraction in t075 is
                           allowed, protocol .c/.h are not)
```

`src/main.c` and `src/cli.{c,h}` are **allowed** to change (engine factory +
`--engine` flag from t073).  `src/scripting/**` is allowed (that is the Request
Layer — the thing under test).

## Deliverables

### 1. `tests/e2e/parity_lua_quickjs.sh`

1. Start a recording Redis mock that appends every received command to a file.
2. Run `--engine=lua -s scripts/redis_get_set.lua -t1 -c1 -d2s -R20`; save capture.
3. Restart mock (clear); run `--engine=quickjs -s scripts/redis_get_set.js` with
   identical flags; save capture.
4. Normalize each capture into a sorted multiset of `(verb,key,value)` commands
   (ordering across runs is not guaranteed), then compare.  Identical ⇒ PASS.
5. `SKIP` cleanly if QuickJS not built.

### 2. `tests/e2e/gate_d.sh`

```sh
#!/bin/sh
set -e
BASELINE="${GATE_D_BASELINE:-p5-baseline}"
PASS=0; FAIL=0
frozen="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h src/net.c src/net.h src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h"

check() { if eval "$2" >/dev/null 2>&1; then echo "PASS  $1"; PASS=$((PASS+1));
          else echo "FAIL  $1"; FAIL=$((FAIL+1)); fi; }

# 1. frozen core/protocol paths unchanged since the P5-2 baseline
check "frozen core+protocol unchanged since $BASELINE" \
      "test -z \"\$(git diff --name-only $BASELINE HEAD -- $frozen)\""

# 2. both engines build
check "LuaJIT build"  "make >/dev/null"
check "QuickJS build" "./configure --with-quickjs && make >/dev/null"

# 3. byte/command parity
check "Lua/JS Redis request parity" "bash tests/e2e/parity_lua_quickjs.sh"

echo ""; echo "Gate D: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
```

If a frozen file legitimately must change, the gate **fails loudly** and the
change must be justified in the ADR — that is the whole point of the gate.

### 3. `Makefile` — `gate-d` target

```makefile
gate-d:
	@bash tests/e2e/gate_d.sh
```

### 4. `docs/adr/0005-next-phases-roadmap.md` — Gate D result section

Append a short "Gate D result" subsection (date, both engine names + versions,
the `gate_d.sh` output), matching the Gate C' write-up pattern.

### 5. `tasks/completed/t076-js-lua-parity-gate-d.md`

Record the actual `make gate-d` output and the resolved `p5-baseline` hash.

## Guards

- `make gate-d` exits 0
- `make test` still green (parity harness adds no regressions)
- `git diff <p5-baseline> HEAD --name-only` touches none of the frozen paths

## Core engine touch

Zero.  Gate D is verification only.
