# ADR backups (not active)

Drafts and superseded-in-place ADR documents kept here for later review. Files
in this directory are **not** part of the active ADR index in
`../README.md` and are not implemented by any task.

## Contents

- `0003-orchestrator-differential-testing-harness.md` — an earlier draft that
  claimed the `0003` slot for a broad "differential testing harness" (rate
  accuracy + calibration latency + reconnect behaviour, compared old-vs-new).
  It was moved here on 2026-06-05 so the `0003` number could be used for the
  focused **Phase-0 Rate-Accuracy & Keep-Alive Close Handling** decision (the
  active `../0003-...md`), which captures a confirmed, root-caused regression.

  The differential-harness idea remains valuable and overlaps with the
  `baseline/` snapshot + `scripts/compare.sh` already in the tree. Revisit it
  as a future ADR (next free number) once the parity fixes land.
