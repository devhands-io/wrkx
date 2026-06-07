title: structured output sections
status: completed
adr: ~
adr-step: ~
depends: ~

## Why / Goal
wrkx output is flat — version, config, calibration bars, stats, and summary run
together with no visual structure. Split it into 5 named sections so output is
easy to scan: Configuration, Calibration, Latency, Latency spectrum, Summary.

Section headers are followed by a terminal-width rule line (falls back to 100
when stdout is not a tty):

  Configuration
  ----------------------------------------------------------------------------------------------------
  wrkx 0.1.5 [kqueue]
  Build:       ./configure --with-lua --with-quickjs
  Built-in:    redis memcached
  ...

## Deliverables

### `./configure`
- Track `CONFIGURE_FLAGS` (non-feature flags: `--with-openssl`, `--prefix`) and
  write it to config.mk. `--with-quickjs` is intentionally excluded — it is
  expressed via `QUICKJS_ENABLED` so the Makefile can compose the canonical
  build string without duplication.

### `Makefile`
- Default `EXTENSIONS` changed from `redis` to `redis memcached`.
- Build `WRKX_CONFIGURE_FLAGS` from Makefile variables: always starts with
  `--with-lua`, appends `--with-quickjs` when `QUICKJS_ENABLED=1`, then any
  non-feature configure flags. Passed as `-DWRKX_CONFIGURE_FLAGS`.
- `WRKX_EXTENSIONS` (the `EXTENSIONS=` value) passed as `-DWRKX_EXTENSIONS`.

### `src/main.c`
- Added `#include <sys/ioctl.h>`, `#include "ae.h"`, `#include <inttypes.h>`.
- `print_rule()`: prints a terminal-width dash line (`ioctl(TIOCGWINSZ)`,
  fallback 100).
- `print_config_section()`: replaces the old 2-line "Running…/N threads" header
  with a full Configuration section:
  - `wrkx X.Y.Z [event-api]`
  - `Build: ./configure --with-lua [--with-quickjs]`
  - `Built-in: <extensions>` (no separate Engine line — engine is in Build)
  - URL, Threads, Connections, Duration, Rate (if set), Script (if set)
  - Threads and Connections on separate lines (not concatenated)
- Prints `Calibration\n<rule>` before calling `orchestrator_run()`.

### `src/orchestrator.c`
- Added `#include <sys/ioctl.h>`.
- `print_rule()` / `print_section(name)` helpers (terminal-width rule).
- Removed `SECTION_RULE` static macro approach.
- `print_stats_header()`: left-aligned columns (`%-14s%-12s…`) instead of
  right-aligned `%Ns` format.
- `print_stats()`: formats values to strings first, then prints left-aligned
  with `%-14s%-12s%-12s%-12s%.2Lf%%`. Removed dependency on `print_units` for
  this path.
- `print_hdr_latency()`: removed `bool print_spectrum` parameter; spectrum is
  now a separate section managed by the caller.
- Removed all leading 2-space indents from every user-facing string (section
  content, progress bars, cal_msg, stats rows, summary lines).
- Progress bars: `"  Calibrating: ["` → `"Calibrating: ["`,
  `"  Progress: ["` → `"Progress: ["`.
- `cal_msg`: removed leading `"  "`.
- Report stage restructured:
  - `print_section("Latency")` before stats header
  - `print_section("Latency spectrum")` + `hdr_percentiles_print()` when
    `-L`/`-U` without `-l`
  - `print_section("Summary")` before Measurement block
  - `Requests/sec` / `Transfer/sec` moved before error lines within Summary
  - Removed old `--…--` separator line
- Removed `printf("\n")` after `pthread_join(progress_thread)` — the leading
  `\n` in `print_section` provides the single blank line gap correctly.

## Guards
- `make` builds clean (no warnings under -Wall -Werror)
- `make test` passes (all 12 cli_output checks, redis, memcached, quickjs)
- `make test-asan` passes
- Frozen files unchanged: `include/wrkx_extension.h`, `src/orchestrator.h`,
  `src/cli.h`

## Core engine touch
- **Allowed:** `src/main.c`, `src/orchestrator.c`, `./configure`, `Makefile`
- **Not allowed:** `include/wrkx_extension.h`, `src/orchestrator.h`,
  `src/cli.h`, `src/proto/`, `src/scripting/`, `extensions/`
