title: fix: progress bar run duration shows 7s instead of 8s for -d18
status: completed

## Observed

With `-c400 -t4 -d18 -R400`, the run progress bar displayed:

```
Progress: [>                   ]   0% (0s / 7s)
```

Expected denominator: `8s` (18s duration − 10s calibration). Actual: `7s`.

## Root cause

The denominator `tot_s` was computed with integer floor division:

```c
uint64_t tot_s = total_us / 1000000;
```

`total_us` is the wall-clock distance from `bar_start` (when phase-3 starts, after
calibration completes) to `stop_at`. The gap is not a clean integer number of
seconds because:

1. With `-c400 -t4` each thread has 100 connections →
   `calibrate_delay = CALIBRATE_DELAY_MS + 100×5 = 10500 ms`
2. Phase 3 starts at `t₀ + ~10.5s`
3. `stop_at = connections_ready_at + duration_us ≈ t₀ + 0.5s + 18s = t₀ + 18.5s`
4. `total_us = 18.5s − 10.5s = 7.995s` (ramp-up consumes ~0.5s from the expected 8s gap)
5. `7995000 / 1000000 = 7` — floor truncation drops 0.995s

## Fix

Use ceiling division for `tot_s` so 7.995s displays as `8s`:

```c
uint64_t tot_s = (total_us + 999999) / 1000000;
```

`el_s` (elapsed) intentionally keeps floor division — you don't want to show `1s`
before a full second has passed.

## Guards

- `make test`, `make test-asan` pass
- Frozen file diff empty (`include/wrkx_extension.h`)
