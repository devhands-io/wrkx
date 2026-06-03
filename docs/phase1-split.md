# Phase 1 — wrk.c God-File Split Plan

## Context

`src/wrk.c` is 955 lines with 35 top-level symbols spanning six distinct concerns.
This document is the authoritative map Phase 1 executes against. **No code is moved
until this doc is approved.** The test suite (make test + make test-asan + make
coverage) must stay green after every individual symbol is relocated.

Snapshot taken from commit `39f52dd` on branch `devel`.

---

## Target Module Map

Every symbol currently in `src/wrk.c` appears in exactly one target below.

### `src/output.c` + `src/output.h` (new files)

Concern: human-readable result formatting; no I/O other than stdout.

| Symbol | Line | Kind | Note |
|--------|------|------|------|
| `print_stats_header()` | 911 | function | |
| `print_units()` | 915 | function | calls `format_*` from units.h |
| `print_stats()` | 928 | function | |
| `print_hdr_latency()` | 940 | function | takes `bool print_spectrum` (our addition) |

Dependencies needed in `output.h`: `stats.h`, `hdr_histogram.h`, `units.h`.
No access to `cfg` or globals — all inputs arrive via parameters. ✓ No changes
needed to make these functions non-static beyond removing the `static` keyword.

---

### `src/cli.c` + `src/cli.h` (new files)

Concern: argument parsing and usage text.

| Symbol | Line | Kind | Note |
|--------|------|------|------|
| `usage()` | 60 | function | prints to stdout/stderr |
| `longopts[]` | 790 | static array | only used by `parse_args` — can stay `static` inside cli.c |
| `parse_args()` | 808 | function | fills `struct config *` passed by pointer |

Dependencies needed in `cli.h`: `wrk.h` (for `struct config`, `struct http_parser_url`).
`cfg` is passed **by pointer** into `parse_args`, so no extern needed. ✓

---

### `src/connection.c` + `src/connection.h` (new files)

Concern: socket lifecycle, HTTP parsing callbacks, write scheduling.

| Symbol | Line | Kind | Note |
|--------|------|------|------|
| `parser_settings` | 50 | static struct | `http_parser_settings`; moves with callbacks |
| `sock` | 42 | static struct | socket dispatch table; moves here |
| `connect_socket()` | 387 | function | |
| `reconnect_socket()` | 419 | function | |
| `delayed_initial_connect()` | 426 | function | ae timer callback |
| `header_field()` | 498 | function | http_parser callback |
| `header_value()` | 508 | function | http_parser callback |
| `response_body()` | 518 | function | http_parser callback |
| `usec_to_next_send()` | 524 | function | called only by rate.c; see cross-module note |
| `delay_request()` | 565 | function | ae timer callback; see cross-module note |
| `response_complete()` | 575 | function | http_parser callback |
| `socket_connected()` | 664 | function | ae event callback |
| `socket_writeable()` | 689 | function | ae event callback |
| `socket_readable()` | 746 | function | ae event callback |

**Cross-module access required:**
- `cfg` (extern) — `response_complete` reads `cfg.record_all_responses`; `socket_writeable`
  reads `cfg.dynamic` and `cfg.pipeline`; `connect_socket` reads `cfg.host`.
- `time_us()` (non-static) — called by `response_complete`, `connect_socket`,
  `socket_writeable`, `socket_readable`.
- `stop` (extern) — not referenced here; lives in wrk.c, read by rate.c only.

**Note on `usec_to_next_send` and `delay_request`:** these two are tightly coupled
to the write/response path and have no dependency on rate.c internals, so they
live in connection.c despite being "rate-adjacent". Alternatively they can move to
rate.c if the Phase 1 implementer prefers; either placement has no circular dep.

---

### `src/rate.c` + `src/rate.h` (new files)

Concern: coordinated-omission calibration, timeout detection, throughput sampling.

| Symbol | Line | Kind | Note |
|--------|------|------|------|
| `calibrate()` | 433 | function | ae timer callback; increments `g_calibrated_threads` |
| `check_timeouts()` | 462 | function | ae timer callback; reads `stop`, `cfg.timeout` |
| `sample_rate()` | 482 | function | ae timer callback; writes `statistics` |

**Cross-module access required:**
- `cfg` (extern) — `check_timeouts` reads `cfg.timeout`.
- `stop` (extern) — `check_timeouts` reads the signal flag.
- `statistics` (extern) — `sample_rate` locks and writes `statistics.requests`.
- `g_calibrated_threads` (extern) — `calibrate` increments it.
- `time_us()` (non-static) — called by `check_timeouts` and `sample_rate`.

No calls into `connection.c` ✓ — `check_timeouts` iterates `thread->cs` directly
without calling socket functions. Dependency graph is acyclic.

---

### `src/wrk.c` (residual — keeps these symbols only)

| Symbol | Line | Kind | Note |
|--------|------|------|------|
| `g_calibrated_threads` | 15 | global `volatile int` | written by rate.c, read by progress_main |
| `g_progress_done` | 16 | global `volatile int` | written and read within wrk.c only |
| `cfg` | 18 | global `struct config` | written by main/parse_args, read everywhere |
| `statistics` | 37 | global struct | written by sample_rate (rate.c), read by main |
| `stop` | 54 | global `volatile sig_atomic_t` | set by handler, read by check_timeouts |
| `handler()` | 56 | function | signal handler; sets `stop = 1` |
| `progress_main()` | 89 | function | pthread; reads `g_calibrated_threads` |
| `main()` | 137 | function | orchestration |
| `thread_main()` | 327 | function | per-thread event loop entry point |
| `time_us()` | 771 | function | must become **non-static** (used by 3 other modules) |
| `copy_url_part()` | 777 | function | used only by `main`; can stay static |

---

## Shared State — Cross-Module Access Summary

| Global | Type | Defined in | Read by | Written by |
|--------|------|------------|---------|------------|
| `cfg` | `struct config` | wrk.c | connection.c, rate.c, cli.c, output.c | main() via parse_args |
| `statistics` | anon struct | wrk.c | main() | rate.c `sample_rate` |
| `stop` | `volatile sig_atomic_t` | wrk.c | rate.c `check_timeouts` | wrk.c `handler` |
| `g_calibrated_threads` | `volatile int` | wrk.c | wrk.c `progress_main` | rate.c `calibrate` |
| `g_progress_done` | `volatile int` | wrk.c | wrk.c `progress_main` | wrk.c `main` |

**Recommended approach:** declare all five in a new `src/globals.h` as `extern`
declarations; define them (without `extern`) in `src/wrk.c`. Each new module that
needs them includes `globals.h`.

---

## Symbols That Must Become Non-Static Before Splitting

Any symbol currently `static` that is referenced by a different target module
must have `static` removed and a declaration added to the appropriate header.

| Symbol | Current | Action | Declare in |
|--------|---------|--------|------------|
| `time_us()` | `static` in wrk.c | remove `static` | `src/wrk.h` or new `src/utils.h` |
| `cfg` | `static struct` in wrk.c | remove `static`; add `extern` decl | `src/globals.h` |
| `statistics` | `static struct` in wrk.c | remove `static`; add `extern` decl | `src/globals.h` |
| `stop` | `static volatile` in wrk.c | remove `static`; add `extern` decl | `src/globals.h` |
| `g_calibrated_threads` | `static volatile` in wrk.c | remove `static`; add `extern` decl | `src/globals.h` |

`copy_url_part()` — only called by `main()`; stays `static`, no change needed.
`longopts[]` — only used by `parse_args()`; stays `static` inside cli.c.

---

## New Header Files Required

| File | Declares |
|------|----------|
| `src/globals.h` | `extern` for cfg, statistics, stop, g_calibrated_threads, g_progress_done |
| `src/output.h` | print_stats_header, print_units, print_stats, print_hdr_latency |
| `src/cli.h` | parse_args, usage |
| `src/connection.h` | connect_socket, reconnect_socket, delayed_initial_connect, socket_connected, socket_writeable, socket_readable, header_field, header_value, response_body, response_complete, usec_to_next_send, delay_request; also `sock`, `parser_settings` |
| `src/rate.h` | calibrate, check_timeouts, sample_rate |

`src/wrk.h` already declares `thread`, `connection`, and constants — it needs
`time_us()` added once the function becomes non-static.

---

## Dependency Graph (no cycles)

```
output.c  ──→  stats.h, hdr_histogram.h, units.h
cli.c     ──→  wrk.h (config struct)
connection.c ──→  wrk.h, globals.h (cfg, time_us), ae.h, http_parser.h, ssl.h, script.h
rate.c    ──→  wrk.h, globals.h (cfg, stop, statistics, g_calibrated_threads, time_us), script.h
wrk.c     ──→  all of the above (orchestration)
```

No back-edges. Verified by tracing every call in `check_timeouts` and
`response_complete` — rate.c does not call into connection.c.

---

## Phase 1 Execution Order

Recommended order to keep the build green at every step:

1. Create `src/globals.h`; remove `static` from cfg, statistics, stop,
   g_calibrated_threads; add `extern` declarations. Build + test.
2. Remove `static` from `time_us()`; add declaration to `src/wrk.h`. Build + test.
3. Create `src/output.c` + `src/output.h`; move 4 functions; add to `SRC` in Makefile. Build + test.
4. Create `src/cli.c` + `src/cli.h`; move usage, longopts, parse_args. Build + test.
5. Create `src/rate.c` + `src/rate.h`; move calibrate, check_timeouts, sample_rate. Build + test.
6. Create `src/connection.c` + `src/connection.h`; move remaining 14 symbols. Build + test.
7. Verify residual `src/wrk.c` contains only the 11 symbols listed above. Build + test.
8. Run `make test && make test-asan && make coverage` — all must pass green.
