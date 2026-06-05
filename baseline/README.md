# baseline/ — frozen original-wrk2 reference

This directory holds a **vendored, frozen snapshot of the original wrk2-derived
`wrk.c` implementation**, before the three-layer orchestrator/engine refactor.
It exists so we can build the *old* binary alongside the *new* one and compare
behaviour while the new architecture is still being validated.

## Provenance

- **Source:** repo commit `505cd14` (`feat: add -l option …`) — the **last
  commit before the live progress bar** (`bbd3d56`) was added.
- `baseline/src/` is the verbatim `src/` tree from that commit (31 files),
  extracted with `git archive 505cd14 -- src`. The binary self-reports
  `wrkx 4.0.0 … Copyright (C) 2012 Will Glozer` — i.e. pristine wrk2 rate logic.

### Why this commit, not the very first one

The original goal was the *root* commit `5daf8ed` ("wrk2 original sources"). Two
things forced a slightly later commit:

1. **Exact rate accuracy.** The progress-bar commit `bbd3d56` joins a progress
   thread *before* computing `runtime_us`, adding a few ms to the denominator —
   so post-`bbd3d56` builds (including `ea8ea9e`, the previous baseline, and the
   refactored `wrkx`) report a hair under the requested rate. Pre-`bbd3d56`
   builds report it **exactly** (verified: `-R3000` → `Requests/sec 3000.13` on
   an idle box). `505cd14` is the newest commit that still measures cleanly.
2. **Buildability here.** The root commit targets LuaJIT 2.0 (`struct luaL_reg`)
   and lacks the non-Linux `cpu_set_t`/affinity guards; it will not compile
   against this repo's LuaJIT 2.1 on macOS without editing the frozen source.
   `505cd14` is past those platform fixes (`f3e2b56`) yet behaviourally
   identical to the original for rate purposes (the rate/measurement code is
   untouched from root through `505cd14`).

So `505cd14` is the earliest commit that both **builds in this environment** and
**reports exact rate** — the true "exact 3000" reference. The earlier history
(`5daf8ed` … `505cd14`) shares the same pacing and runtime-measurement code.

- **Dependencies are shared, not vendored:** the baseline build links the
  parent tree's `../deps/luajit` (2.1) and reads OpenSSL flags from
  `../config.mk` (run `./configure` once at the repo root).

## Freeze policy (do not edit baseline/src/)

The phase-0 code under `baseline/src/` is the behavioural reference. It **must
stay byte-for-byte unchanged** until the new orchestrator/engine architecture
is fully tested. It is guarded by a checksum manifest:

- `baseline/MANIFEST.sha256` — SHA-256 of every file under `baseline/src/`.
- `baseline/verify.sh` (`make baseline-verify`) — recomputes and compares;
  exits non-zero if any frozen file is edited, added, or removed.

`make baseline-verify` runs as the first step of `make test` and in CI, so any
accidental edit to the frozen code fails the build immediately.

Everything *outside* `baseline/src/` (this README, `baseline/Makefile`,
`baseline/verify.sh`) is baseline **tooling**, authored for the harness, and may
evolve — it is not part of the frozen manifest.

## Building & comparing

```sh
make baseline            # builds baseline/wrkx0 (phase-0 binary)
make baseline-verify     # confirm frozen code is untouched
make compare             # old vs new, instant server, default args
make compare MODE=close  # old vs new against a Connection: close server

# custom args (passed to BOTH binaries):
scripts/compare.sh delay -- -t2 -c8 -d8s -R1000 -L
```

`scripts/compare.sh` runs `baseline/wrkx0` and `./wrkx` against the same
`tests/e2e/mock_server.py` mode and asserts the new binary's Requests/sec is
within tolerance (default 25%) of the old one.

## When the new architecture is validated

Once the new engine fully supersedes phase-0, this directory and its `make`
targets can be removed in a single commit, and the manifest guard retired.
