# ADR-002: Orchestrator Differential Testing Harness

| Field         | Value                                       |
|---------------|---------------------------------------------|
| Status        | Accepted                                    |
| Date          | 2026-06-05                                  |
| Deciders      | wrkx core team                              |
| Depends on    | ADR-001 (three-layer architecture)          |
| Audience      | Claude Code (implementing agent)            |

> This ADR is an implementation brief. Tasks, interfaces, schemas, and
> acceptance criteria are written to be executed directly. Where a
> signature or file path is given, use it verbatim unless a stated
> invariant forces a change.

---

## Context

wire-level request comparison (ADR-001 era tooling) verifies *what bytes
wrkx sends*. That is necessary but covers the easy class of bug. The
expensive, hard-to-find regressions live in the orchestrator's dynamic
behavior:

- **Rate accuracy.** Requested 3000 rps: the legacy build issues exactly
  3000; the refactored build issues ~2900.
- **Calibration latency.** The refactored build reports slightly higher
  latency during the calibration phase than legacy under identical load.
- **Reaction to connection close / reconnect.** Behavior under abrupt
  server close, and the cost/correctness of reconnection.
- **Coordinated Omission correction.** The latency-correction math that
  is the core reason wrk2/wrkx exists.

These are emergent properties of the rate loop and the correction math
over time. They do not crash, do not change the request bytes, and are
masked by measurement noise that frequently exceeds the size of the
regression itself. A naive "run new, eyeball the numbers" approach cannot
reliably separate a real 3% rate regression from a 5% noisy run.

The two prior options were inadequate here:

- A frozen *reference binary* compared externally cannot isolate
  environmental drift between two separate process launches.
- A function-paired *shadow* switch is impossible: the deep refactor
  removes the function correspondence the switch would need.

---

## Decision

Build a **single binary** that contains both the fully isolated legacy
orchestrator and the new orchestrator, can run either in isolation, and
can run a self-contained **paired A/B differential** that detects
systematic differences in dynamic metrics with statistical rigor.

Localization of detected regressions is provided by (a) the granularity
of the tracked metric set and (b) deterministic property tests on the
extracted rate-control and CO-correction modules.

### Tracked metrics (mandatory)

| Stage       | Metrics                                            |
|-------------|----------------------------------------------------|
| Calibration | latency (p50, p99, mean), RPS                      |
| Main        | latency (p50, p99, p99.9, mean, max), RPS, bytes   |

Plus, for every run: requested RPS, completed requests, error counts by
class (connect, read, write, timeout, status).

---

## Architecture

### 1. Hermetic legacy unit

The legacy orchestrator and everything it transitively needs is copied,
unmodified, into `legacy/`. It is **self-contained**: it includes its own
copies of every header it uses. Its include path resolves only to
`legacy/` — it must never compile against an evolving `src/` header,
because that would silently change its behavior while its source hash
stayed constant.

```
legacy/
  *.c  *.h          verbatim copy of pre-refactor source + ALL its headers
  SHA256SUMS        covers every file in legacy/, committed
```

Symbol collisions with `src/` are resolved at the object level so the
legacy source is never edited:

```makefile
LEGACY_OBJ := obj/legacy_orchestrator.o
$(LEGACY_OBJ): $(wildcard legacy/*.c) legacy/SHA256SUMS
	cd legacy && sha256sum -c SHA256SUMS      # hard fail on any edit
	$(CC) $(CFLAGS) -Ilegacy -c legacy/*.c -o $@.tmp
	objcopy --prefix-symbols=legacy_ $@.tmp $@

check-legacy:
	cd legacy && sha256sum -c SHA256SUMS

all: check-legacy
```

Invariant L1: `legacy/` files are byte-identical to the SHA256SUMS
manifest at every build. A mismatch fails the build.
Invariant L2: no file in `legacy/` includes any header outside `legacy/`.
Invariant L3: legacy source is never edited. Symbol renaming is
post-compilation (`objcopy`) only.

The legacy orchestrator runs with the real clock and real I/O, exactly
as it shipped. Its existing stats structures are read after each run
(via the `legacy_`-prefixed symbols) and serialized to the metrics
schema below. No legacy logic is modified to produce metrics.

### 2. Single binary, run modes

```
wrk --impl=legacy  [normal wrk args]        run legacy pipeline only
wrk --impl=new     [normal wrk args]        run new pipeline only (default)
wrk --impl=compare --scenario=<s> --trials=N --out=<f>
                                            run paired A/B, emit records
```

`--impl=compare` is the differential harness. It never targets a live
user server; it drives the bundled deterministic mock server (section 4).

### 3. Metrics schema

Both pipelines emit one JSON object per run to the `--out` file (JSON
Lines). Schema is identical across impls so the analyzer can pair them:

```json
{
  "impl": "legacy",
  "scenario": "fixed_50ms",
  "trial": 7,
  "requested_rps": 3000,
  "duration_s": 10,
  "calibration": {
    "rps": 2998.4,
    "latency_us": { "p50": 50100, "p99": 51200, "mean": 50180 }
  },
  "main": {
    "rps": 2900.1,
    "completed_requests": 28999,
    "bytes": 1234567,
    "latency_us": { "p50": 50050, "p99": 51900, "p999": 60100,
                    "mean": 50210, "max": 71000 },
    "errors": { "connect": 0, "read": 3, "write": 0,
                "timeout": 0, "status": 0 }
  }
}
```

Interface to implement (both pipelines populate it):

```c
/* metrics.h */
typedef struct {
    double   rps;
    uint64_t lat_p50_us, lat_p99_us, lat_mean_us;
} stage_metrics;

typedef struct {
    double   rps;
    uint64_t completed_requests, bytes;
    uint64_t lat_p50_us, lat_p99_us, lat_p999_us, lat_mean_us, lat_max_us;
    uint64_t err_connect, err_read, err_write, err_timeout, err_status;
} main_metrics;

typedef struct {
    const char    *impl;        /* "legacy" | "new" */
    const char    *scenario;
    int            trial;
    uint64_t       requested_rps;
    uint64_t       duration_s;
    stage_metrics  calibration;
    main_metrics   main;
} run_record;

void metrics_emit(FILE *out, const run_record *r);   /* writes one JSONL line */
```

### 4. Deterministic mock server

Bundled, controllable, so the only variable between legacy and new is the
client orchestrator. Scenarios:

| Scenario        | Behavior                                                |
|-----------------|---------------------------------------------------------|
| `fixed_50ms`    | every response delayed exactly 50 ms                    |
| `fixed_1ms`     | every response delayed exactly 1 ms (stresses rate math)|
| `abrupt_close`  | closes the connection after a random subset of requests |
| `slow_start`    | first 2 s delayed 200 ms, then 5 ms (calibration stress)|
| `error_10pct`   | 10% of responses return 503                             |

Each scenario × {requested rate ∈ 1000, 3000, 10000} forms the run matrix.

### 5. Comparison methodology (noise control is the point)

A 3% regression must be detected through noise that is often larger.
This is achieved by pairing and interleaving, not by tolerance widening.

1. **Interleave** trials: `legacy, new, legacy, new, …` so slow
   environmental drift (thermal, background load) affects both equally.
2. **Pair** each `legacy_i` with the adjacent `new_i`; analyze the
   *paired differences* `d_i = metric(new_i) − metric(legacy_i)`. Paired
   deltas cancel per-trial environmental variance and are far tighter
   than independent comparison.
3. **Repeat** `--trials=N` (default 30) per scenario × rate cell.
4. **Pin** both pipelines to the same isolated CPU set (wrkx already
   supports affinity); discard the first 2 trials as warmup.
5. **Analyze** per (scenario, rate, stage, metric): report
   `mean(d)`, relative delta vs `mean(legacy)`, and the 95% CI
   (`mean(d) ± 1.96·sd(d)/√N`).
6. **Verdict**: a regression is flagged when the CI excludes zero **and**
   the relative delta exceeds the per-metric noise floor (default 0.5%
   for RPS and bytes, 2% for latency percentiles).

The analyzer is a Python script reading the JSONL; the binary's compare
mode only runs trials and emits records.

### 6. Localization — without instrumenting legacy internals

The tracked metric set is itself the localizer. The divergence pattern
points at the subsystem:

| Divergence pattern                                | Implicated subsystem            |
|---------------------------------------------------|---------------------------------|
| Calibration RPS diverges                          | calibration / warmup logic      |
| Calibration RPS matches, main RPS diverges        | steady-state rate controller    |
| RPS matches, latency diverges                     | CO correction or measurement    |
| Latency matches, bytes diverge                    | response handling / accounting  |
| Divergence only under `abrupt_close`              | reconnect path                  |
| Divergence only under `slow_start`                | calibration adaptation          |

For exact, code-level localization of rate and CO math bugs, the new
code's extracted modules are tested deterministically (section 7,
secondary). These need no legacy code.

### 7. Deterministic property tests (secondary, localizes math bugs exactly)

ADR-001 extracts the rate controller and CO correction as pure-ish
modules in the orchestrator. Test them against invariants that pin the
exact behavior the legacy code exhibits:

- **Rate exactness under ideal conditions.** Given responses that all
  complete instantly relative to the schedule, for rate R over duration
  D the controller MUST schedule exactly `R·D` requests. The 3000→2900
  bug fails this property directly, deterministically, at unit level.
- **CO correction monotonicity & known-input vectors.** For a
  hand-constructed sequence of (scheduled, actual) send times, the
  corrected histogram MUST equal a precomputed golden vector.
- **Calibration convergence.** Given a fixed synthetic latency, the
  calibration output MUST be stable across repeated runs (no clock or
  RNG dependence in the math itself).

These are exact (zero tolerance) because they exercise the math with no
real clock or I/O.

---

## Implementation Sequence

```
T-D1  Create hermetic legacy/ + SHA256SUMS + check-legacy hard gate
T-D2  objcopy symbol-prefix rule; legacy links into the single binary
T-D3  metrics.h + metrics_emit; new pipeline populates run_record
T-D4  legacy adapter: read legacy_ stats structs → run_record (no legacy edit)
T-D5  --impl={legacy,new,compare} dispatch
T-D6  bundled deterministic mock server with the 5 scenarios
T-D7  compare mode: interleaved paired trials, JSONL emission
T-D8  Python analyzer: paired stats, per-cell verdict, regression report
T-D9  extract rate + CO modules in new code; deterministic property tests
T-D10 wire `make test-orch-diff` (compare mode + analyzer) into the gate
```

T-D1..T-D2 are prerequisites for everything. T-D9 is independent of the
legacy harness and can proceed in parallel after ADR-001's orchestrator
extraction.

---

## Acceptance Criteria

1. `make check-legacy` fails the build if any `legacy/` file differs from
   `SHA256SUMS`. (Verify by mutating one byte and confirming build fails.)
2. `grep -rL 'legacy/' legacy/*.c` style check: no `legacy/` file includes
   a header outside `legacy/`. (Invariant L2, mechanically checked.)
3. `wrk --impl=legacy` and `wrk --impl=new` each run a normal load test
   and emit a valid `run_record`.
4. `make test-orch-diff` runs the full scenario × rate matrix and produces
   a per-cell regression report with paired CIs.
5. **Reproduction gate (the real test):** with the current refactored
   orchestrator, `make test-orch-diff` MUST flag the known rate regression
   — `main.rps` relative delta ≈ −3% on `fixed_1ms` @ 3000, CI excluding
   zero — and MUST flag the calibration latency regression. If the harness
   does not detect the bug that motivated this ADR, the harness is
   incomplete.
6. The deterministic rate-exactness property test (T-D9) fails on the
   current new rate controller and passes once the rate bug is fixed.
7. After the fix, `make test-orch-diff` reports no flagged regression
   across the matrix (all CIs within noise floor).

---

## Consequences

### Positive

- Detects systematic dynamic regressions (rate, latency, CO) that wire
  and output tests structurally cannot see.
- Single binary + interleaved pairing removes cross-process and drift
  confounders that defeated the separate-binary approach.
- Legacy reference is hermetic and hash-gated: its source cannot drift
  unnoticed, and it never recompiles against new headers.
- Metric-pattern localization narrows a flagged regression to a subsystem
  with no instrumentation of legacy internals.
- Deterministic property tests pin the rate/CO math exactly and outlive
  the legacy harness.

### Negative

- The legacy unit recompiles on every build and bloats the binary until
  removed. Mitigated by `check-legacy` being cheap and the deletion plan.
- Statistical comparison requires many trials; the matrix run is minutes,
  not seconds. It belongs in a dedicated gate, not the per-edit loop.
- Noise control depends on CPU pinning and a quiet host; CI runners with
  noisy neighbors widen CIs and can mask sub-noise-floor regressions.
  The analyzer must report achieved CI width so a too-noisy run is
  visibly inconclusive rather than falsely green.

### Neutral / lifecycle

- `legacy/`, the `objcopy` rule, `--impl=legacy`, and the compare mode are
  removed in one commit once the orchestrator is verified and stable. The
  deterministic property tests (T-D9) are permanent.

---

## Compliance

A change violates this ADR if:

- any `legacy/` file is edited (source must change only via a reviewed
  `SHA256SUMS` regeneration, which is itself a visible diff);
- a `legacy/` file includes a header from `src/`;
- the compare harness widens tolerances instead of adding trials to reach
  significance;
- a regression report is treated as green when the achieved CI width
  exceeds the noise floor (inconclusive ≠ pass).

Mechanical checks:

```sh
# legacy integrity
cd legacy && sha256sum -c SHA256SUMS || echo VIOLATION

# legacy hermeticity (no src/ headers pulled in)
grep -rn '#include' legacy/ | grep -v 'legacy/' | grep '"' && echo VIOLATION
```

