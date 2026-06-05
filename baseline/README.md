# baseline/ — frozen phase-0 reference

This directory holds a **vendored, frozen snapshot of phase-0 wrkx** (the
original `wrk.c`-based implementation, before the three-layer
orchestrator/engine refactor). It exists so we can build the *old* binary
alongside the *new* one and compare behaviour while the new architecture is
still being validated.

## Provenance

- **Source:** repo commit `ea8ea9e` (`docs(tasks): add Phase 1 tasks …`), the
  last commit before the P1-1 layer split.
- `baseline/src/` is the verbatim `src/` tree from that commit (31 files),
  extracted with `git archive ea8ea9e -- src`.
- **Dependencies are shared, not vendored:** the baseline build links the
  parent tree's `../deps/luajit` and reads OpenSSL flags from `../config.mk`
  (run `./configure` once at the repo root). LuaJIT is vendored upstream and
  effectively stable, so baseline behaviour does not drift in practice.

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
