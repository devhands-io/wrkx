# t078 — Measurement window consistency and UX improvements

## Problem

Reported `Requests/sec` was systematically below `-R` for short durations, and all
metrics were drawn from inconsistent time windows.

Three separate issues:

1. **Ramp-up deficit** — connections are staggered 5 ms apart, so the first
   `connections_per_thread × 5 ms` (~500 ms for `-c400 -t4`) the pool is only
   partially open. Requests sent during that window counted toward `complete` and
   `elapsed`, dragging RPS below target. A fixed deficit of ~2 300 requests divided
   by a short duration produces a large percentage error (`-d11` → −2.2 %,
   `-d15` → −1.6 %).

2. **Inconsistent windows** — `complete`/`bytes` covered `[workers_ready, stop_at]`;
   the latency histogram covered `[calibration_reset, stop_at]` (~10.5 s in);
   `elapsed` was measured from after `pthread_join`, which overshoots `stop_at` by
   up to one response latency. Three different datasets, one set of summary numbers.

3. **Opaque output** — the single `"N requests in Xs"` line did not distinguish the
   measurement window from the full test run, making it impossible to audit what
   the RPS calculation was actually based on.

## Solution

### Connection-ready barrier

Added `signal_connection_ready()` — called on every terminal initial-connect
outcome (success or `connect_abandoned`). It increments `connections_established`;
when the counter reaches `total_connections` it writes the current timestamp to
`connections_ready_at` (volatile, released with `__ATOMIC_RELEASE`).

The main thread spins on `connections_ready_at` after the `workers_ready` barrier,
then:
- sets `run_start = connections_ready_at`
- shifts `stop_at = run_start + duration_us` (threads get the full `-d` of
  steady-state load)
- captures `workers_start_us` for ramp-up duration reporting

### Consistent gating in `record_response`

All four measurement operations are gated on a single `measuring` bool:

```c
bool measuring = (connections_ready_at != 0) && !o->stop
               && (now < stop_at);
```

- `t->complete++`, `t->requests++`, `t->bytes +=` — only when measuring
- `hdr_record_value(latency)`, `hdr_record_value(u_latency)` — only when measuring

Rate-controller state (`rc->complete`, `c->in_flight`) updates unconditionally so
pacing is correct during ramp-up.

### Calibration histogram reset removed

Histograms are already clean (first sample arrives after `connections_ready_at`),
so the `hdr_reset` calls in `calibrate()` were dropped. Resetting them at ~10.5 s
was creating a window mismatch: `complete` covered `[connections_ready_at, stop_at]`
but the histogram only covered `[calibration_reset, stop_at]`.

### Elapsed capped at `duration_us`

```c
if (elapsed > duration_us) elapsed = duration_us;
```

Threads overshoot `stop_at` by up to one response latency (the stop check fires at
completion, not mid-flight). Capping keeps the denominator exact and eliminates the
"300010 requests in 30.01s" artifact where `complete / elapsed_displayed` ≠ reported
RPS.

### Ramp-up tracking

Added `t->ramp_complete` / `t->ramp_bytes` per-thread counters incremented when
`connections_ready_at == 0`, used only for the `Total` output line.

### Two-line summary output

```
  Measurement: 300000 requests in 30.00s, 244.00MB read
  Total:       302347 requests in 30.50s, 244.37MB read (+ 0.50s ramp-up, 2347 req excluded)
Requests/sec:  10000.00
Transfer/sec:      8.13MB
```

`Measurement` is the window used for all math. `Total` shows the complete picture
so the excluded data is auditable.

## Files changed

- `src/orchestrator.c`
