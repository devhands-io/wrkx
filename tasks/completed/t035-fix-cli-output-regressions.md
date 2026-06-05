title: Fix five CLI output regressions introduced in t032
status: completed
adr: 0001
adr-step: P1-5
depends: t032

## Context

Five regressions were introduced during t032 (the P1-5 wiring task) when
`src/cli.c` and `src/main.c` were written to replace `src/wrk.c` as the
active entry point.  The t032 E2E suite only exercises functional paths;
it does not cover `-v` output, latency flag semantics, the progress bar,
or the output section ordering — so all five slipped through CI.

## Regressions

### 1. Credits stripped from `-v` output

Legacy `wrk.c` (post commit `1a8e691`):
```
wrkx 0.1.0 [kqueue] Credits: Will Glozer (wrk), Gil Tene (wrk2)
```
New `cli.c`:
```
wrkx 0.1.0
```
`aeGetApiName()` and the credits line were not ported.

### 2. `-l` / `--l_latency` option dropped

Legacy had two latency flags:
- `-L` (`--latency`) → HdrHistogram WITH full detailed spectrum
- `-l` (`--l_latency`) → HdrHistogram WITHOUT detailed spectrum
  (`latency_dist_only = true`)

`cli.c` only has `-L` with no `latency_dist_only` concept.

### 3. `-L` now behaves like the old `-l` (no detailed spectrum)

`main.c` hand-rolls a percentile loop without calling `hdr_percentiles_print`,
so `-L` never shows the detailed percentile spectrum.  No option currently
produces it.

### 4. Latency Distribution printed after the request summary (wrong order)

Correct order:
```
  Thread Stats     Avg    Stdev      Max   +/- Stdev
    Latency       ...
    Req/Sec       ...
  Latency Distribution (HdrHistogram - Recorded Latency)   ← must be here
    50.000%  ...
  N requests in Xs, XB read
Requests/sec: ...
Transfer/sec: ...
```
Current: `orchestrator_run()` prints Thread Stats + summary all at once;
`main.c` then prints Latency Distribution **after** Requests/sec.

### 5. Progress bar / calibration messages absent

`wrk.c` ran a `progress_main()` thread showing:
```
  Thread calibration: mean lat.: 1.2ms, rate sampling interval: 10ms
  [============================>] 10s
```
`orchestrator.c` buffers `cal_msg` per thread but never prints them.
`main.c` never launches a progress thread.

## Steps

1. **`src/cli.h`**: add `latency_dist_only` bool to `cli_args`.
2. **`src/cli.c`**:
   - Add `{ "l_latency", no_argument, NULL, 'l' }` to longopts.
   - Add `'l'` to the getopt string; handle `case 'l'`:
     `out->latency = true; out->latency_dist_only = true;`
   - Add `-l` entry to `cli_usage()`.
   - Fix `case 'v'`: print `wrkx %s [%s] Credits: Will Glozer (wrk), Gil Tene (wrk2)\n`
     using `aeGetApiName()` (include `ae.h`).
3. **`src/orchestrator.h`**: add `latency`, `latency_dist_only` bool fields
   to `orchestrator_cfg`; add `progress_fn` callback or integrate progress
   directly (see below).
4. **`src/orchestrator.c`**: split the report stage — print Thread Stats first,
   then the Latency Distribution (if `cfg.latency`), then the request summary.
   Also flush `cal_msg` lines and print the progress bar from within the
   orchestrator (progress thread launched in `orchestrator_run`).
5. **`src/main.c`**: wire `args.latency` / `args.latency_dist_only` into
   `orchestrator_cfg`; remove the hand-rolled Latency Distribution block
   (now owned by the orchestrator).

## Acceptance

- `wrkx -v` prints: `wrkx 0.1.0 [kqueue/epoll] Credits: Will Glozer (wrk), Gil Tene (wrk2)`
- `wrkx` (no args) / `wrkx -h` shows `-l` in the usage block.
- Running with `-L` shows the full HdrHistogram including detailed percentile
  spectrum; running with `-l` shows it without the spectrum.
- Output sections appear in the correct order: Thread Stats → Latency
  Distribution (if requested) → request summary → Requests/sec.
- Calibration progress bar appears during the calibration phase and erases
  cleanly before results are printed.
- All existing E2E tests remain green.
- `make test` exits 0.
