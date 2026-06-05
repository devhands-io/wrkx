# 3. Phase-0 Rate-Accuracy & Keep-Alive Close Handling

| Field         | Value                        |
|---------------|------------------------------|
| Status        | Accepted                     |
| Date          | 2026-06-05                   |
| Phase         | Phase 1                      |
| Deciders      | wrkx core team               |
| Supersedes    | —                            |
| Superseded by | —                            |
| Amends        | [0001](0001-three-layer-engine-architecture.md) §"Interface contracts" (proto_status), §"Orchestrator" (run lifecycle) |

> This ADR is an implementation brief. Tasks, interfaces, and acceptance
> criteria are written to be executed directly. Where a signature or file path
> is given, use it verbatim unless a stated invariant forces a change.

---

## Context

After the three-layer refactor (ADR 0001/0002), the new `wrkx` no longer matches
the phase-0 `wrk.c` binary on two observable behaviours. Both were confirmed by
running the **frozen phase-0 baseline** (`baseline/wrkx0`, commit `ea8ea9e`)
against the new binary on the reported environment — a free box with a static
nginx on `localhost:80`, url `/`, `-c28 -t27 -d20 -R3000`:

| | requests | Requests/sec | elapsed (req÷rps) | socket errors |
|---|---|---|---|---|
| OLD `wrkx0` | 60023 | **3000.17** (idle box) | 20.006s | none |
| NEW `wrkx`  | 60021 | **2989.64**            | 20.077s | **read 54** |

The request **count is identical** (~60021) — open-model pacing is correct.
There are two distinct, independently fixable defects.

### Defect A — reported Requests/sec is low because elapsed includes setup

The rate gap is purely a **denominator** error. NEW divides the (correct)
request count by an elapsed clock that starts **before** worker-thread creation,
whereas phase-0 starts it **after**.

Phase-0 `wrk.c` uses two timestamps:

```c
/* wrk.c */
uint64_t stop_at = time_us() + duration;   /* (1) anchored BEFORE the create loop */
... create 27 threads (event loops, LuaJIT VMs, pthread_create) ...
uint64_t start   = time_us();               /* (2) runtime clock, AFTER creation   */
... join threads, join progress ...
uint64_t runtime_us = time_us() - start;    /* denominator excludes setup          */
```

New `orchestrator.c` collapses both into one timestamp:

```c
/* orchestrator.c — orchestrator_run() */
o->start_us = time_us();                     /* used for BOTH stop_at AND elapsed   */
uint64_t stop_at = o->start_us + duration_us;
... create threads ... join ...
uint64_t elapsed = time_us() - o->start_us;  /* denominator INCLUDES setup          */
```

Because the numerator (requests delivered in `[connect → stop_at]`) and the
denominator (`[start_us → end]`) now span different windows, NEW charges thread/
event-loop creation time to the test window. The magnitude scales with thread
count and box speed (≈71 ms for 27 threads on the idle reporter box; ≈8–16 ms on
faster boxes). The result: a steady, environment-dependent under-report
(`2989.64` instead of `3000`).

### Defect B — a graceful keep-alive close is mis-counted as a read error

nginx closes each keep-alive connection after `keepalive_requests` (default
**1000**) responses, sending `Connection: close` on the final one. Confirmed by
arithmetic: observed read errors ≈ total_requests ÷ 1000 (10 s run → 27 errors;
20 s run → 54 errors).

- **Phase-0**: `response_complete` checks `http_should_keep_alive(parser)`; on
  `false` it calls `reconnect_socket()` — a *clean* reconnect, **0 errors**.
- **New**: `http1_readable` records `s->keep_alive` (set by `on_message_complete`)
  but **never propagates it**. It returns `PROTO_DONE`; the orchestrator re-arms
  the socket nginx is closing; the following EOF returns `PROTO_ERROR` →
  `t->errors.read++` + a reconnect with churn. This produces the 54 read errors,
  a fat latency tail (p99.9 12 ms vs 3 ms), and a small additional time cost.

This is the same keep-alive-state loss that caused the t036 phantom-completion
flood, resurfacing on a real keep-alive server. The root contract gap: the
`proto_status` enum has no value meaning *"response complete — and the peer is
closing, reconnect cleanly."* ADR 0001/0002 deferred keep-alive as "a wiring
concern"; it is now a correctness concern.

---

## Decision Drivers

- **Behavioural parity with phase-0** is the acceptance bar for the new
  architecture (see `baseline/` policy). RPS and socket-error counts must match
  the frozen reference within tolerance on the same box/server.
- **Minimal, contract-respecting changes.** No change to the frozen
  `struct connection`; reuse the existing additive-enum pattern from t036.
- **Regression-proof.** Each fix lands with a test that fails before it.

---

## Decision

### A. Two-timestamp measurement window (`src/orchestrator.c`)

Keep `o->start_us` solely as the `stop_at` anchor. Add a second timestamp
captured **after** the worker-creation loop (before the join loop) and report
elapsed/throughput from it:

```c
/* orchestrator_run(), after the pthread_create loop, before the join loop */
uint64_t run_start = time_us();           /* matches wrk.c's post-creation `start` */
... join workers; join progress ...
uint64_t elapsed = time_us() - run_start; /* denominator excludes setup            */
o->result.start_us   = run_start;
o->result.elapsed_us = elapsed;
```

`stop_at` stays anchored at `o->start_us` (pre-creation) so the test still stops
at a fixed wall-clock duration. Only the *reported* denominator moves to align
with the work window. No public signature changes.

### B. Surface keep-alive close through the protocol contract

Add an additive `proto_status` value (mirrors the t036 `PROTO_DONE_STATUS_ERR`
pattern — no struct change):

```c
/* src/proto/proto.h */
typedef enum {
    PROTO_PENDING,
    PROTO_DONE,
    PROTO_DONE_STATUS_ERR,
    PROTO_DONE_CLOSE,      /* NEW: response complete; peer is closing — reconnect, no error */
    PROTO_ERROR
} proto_status;
```

`http1_readable`, when a response completes, returns `PROTO_DONE_CLOSE` instead
of `PROTO_DONE` when `!s->keep_alive` (i.e. `http_should_keep_alive` was false or
the response carried `Connection: close`). The non-2xx case still takes priority
as today.

`orchestrator.c::socket_readable` handles the new value: record the completed
response exactly as `PROTO_DONE` (count it, record latency, call the script
hook), then **reconnect cleanly without incrementing `errors.read`** — the phase-0
`reconnect_socket()` semantics.

This eliminates the 54 spurious read errors and the latency tail, and removes the
reconnect churn that perturbs timing.

---

## Considered Options

- **A — measure elapsed from earliest connection `thread_start`.** More precise
  but needs a cross-thread minimum and touches per-connection state. Rejected:
  the two-timestamp approach is the faithful phase-0 behaviour and far simpler.
- **B — treat *any* EOF-immediately-after-completion as a clean reconnect** (no
  enum change). Rejected: relies on event ordering/timing and hides genuine mid-
  stream resets; the explicit `PROTO_DONE_CLOSE` keeps the protocol the single
  source of truth for keep-alive, consistent with Invariant 2.

---

## Implementation Sequence

```
t037  Defect A — two-timestamp elapsed window in orchestrator.c        (no contract change)
t038  Defect B — PROTO_DONE_CLOSE + http1 keep-alive + clean reconnect (proto.h additive enum)
t039  Parity verification — extend scripts/compare.sh + mock_server keepalive-limit mode;
      assert NEW rps within tolerance of OLD and NEW socket-errors == OLD on the same server
```

t037 and t038 are independent and may land in either order; t039 depends on both.

---

## Consequences

### Positive

- Reported Requests/sec matches phase-0 on an idle box (→ 3000 for `-R3000`).
- Server-initiated keep-alive closes no longer count as errors; latency tail and
  reconnect churn disappear; output matches phase-0.
- `PROTO_DONE_CLOSE` finally gives protocols a way to express keep-alive state,
  closing the gap ADR 0001/0002 deferred — reusable by future protocols.

### Negative

- A fourth `proto_status` value: every `readable` consumer's switch must handle
  it (currently only `orchestrator.c::socket_readable` and the unit tests).
- The elapsed change slightly *increases* reported rps relative to the current
  (buggy) build; any baseline numbers captured pre-fix must be re-taken.

### Neutral

- Calibration-phase latency differences noted in the backed-up differential-
  harness draft are **out of scope** here; revisit separately.

---

## Compliance

After this ADR, a pull request regresses parity if any of the following are true:

- `orchestrator_run` computes reported `elapsed`/`Requests/sec` from a timestamp
  taken before worker-thread creation.
- A protocol detects a non-keep-alive response completion but returns `PROTO_DONE`
  (losing the close signal), or the orchestrator increments `errors.read` for a
  `PROTO_DONE_CLOSE`.
- `scripts/compare.sh` against a keepalive-limited static server shows NEW rps
  outside tolerance of OLD, or NEW socket-error count exceeding OLD's.

The ADR 0001/0002 invariants and grep checks are unchanged.
