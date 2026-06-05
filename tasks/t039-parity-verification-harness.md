title: Parity verification — old-vs-new rps + socket-error gate
status: todo
adr: 0003
adr-step: verify
depends: t037, t038

## Context

ADR 0003 §"Compliance". Lock in the t037/t038 fixes with an automated old-vs-new
comparison so the rps-accuracy and keep-alive-close regressions cannot return.
Reuses the frozen `baseline/wrkx0` (phase-0) and `scripts/compare.sh` already in
the tree. The driver is the keep-alive *close* case (nginx `keepalive_requests`),
which exposes both defects at once.

## Scope

- `tests/e2e/mock_server.py` — add a keepalive-limited mode.
- `scripts/compare.sh` — gate socket-error parity in addition to rps.
- `tests/e2e/` + Makefile/CI wiring for the new comparison.

## Steps

1. `mock_server.py`: add a `kalimit` mode — serves `Connection: keep-alive` but
   closes each connection after N responses (default 1000, like nginx
   `keepalive_requests`), sending `Connection: close` on the Nth. This
   reproduces the 54-read-error scenario deterministically without nginx.
2. `scripts/compare.sh`: in addition to the existing rps tolerance check, parse
   the `Socket errors:` line for both binaries and **fail if NEW read/total
   socket errors exceed OLD's**. Tighten the rps tolerance for clean modes
   (instant/kalimit) to ~1–2% now that the elapsed window is fixed.
3. Add an E2E wrapper (e.g. `tests/e2e/parity.sh`) that builds both binaries,
   runs `compare.sh` for `instant` and `kalimit`, and is invoked from
   `make compare` / a CI step. Keep it off the default `make test` if runtime is
   a concern; otherwise include it.

## Acceptance

- `scripts/compare.sh kalimit` PASSES: NEW rps within tolerance of OLD **and**
  NEW socket-error count ≤ OLD (0). The same comparison FAILS if t037 or t038 is
  reverted (verify by stashing each fix).
- `mock_server.py kalimit` reliably produces ~`requests/1000` connection closes.
- CI runs the parity comparison; `make test` stays green.
- `baseline-verify` green (frozen phase-0 code untouched).
