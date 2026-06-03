title: Calibration phase progress bar
status: completed
depends: t10

Context:
- CALIBRATE_DELAY_MS = 10000ms. During those 10 seconds the user sees only
  "Thread calibration: mean lat.: X, rate sampling interval: Xms" lines with
  no indication of total duration or how far along calibration is.
- The existing progress_main pthread WAITS silently for all threads to calibrate
  before showing the run bar. That wait is the 10s dead zone.
- The calibration() callback prints each update with a trailing \n, so the
  display uses scrolling output during this phase (not \r overwrite). The
  calibration bar must use the same scrolling style to avoid display corruption.

Desired output during calibration (one new line per second, interleaved with
calibration messages):

  Calibrating: [>                   ] 0% (0s / 10s)
  Thread calibration: mean lat.: 1.445ms, rate sampling interval: 10ms
  Calibrating: [==>                 ] 10% (1s / 10s)
  Thread calibration: mean lat.: 1.483ms, rate sampling interval: 10ms
  Calibrating: [=====>              ] 20% (2s / 10s)
  ...
  Calibrating: [====================] 100% (10s / 10s)

  Progress: [>                   ] 0% (0s / 20s)     ← run bar (existing, \r)

Implementation — modify progress_main() in src/wrk.c only:

  Split progress_main into two phases:

  Phase 1 — Calibration bar (new):
    - Print one bar line per second using printf("...\n") (NOT \r).
      Using \n avoids corrupting the interleaved "Thread calibration:" lines.
    - Bar format (20-char wide, matching the run bar style):
        "  Calibrating: [=====>              ] %3d%% (%"PRIu64"s / %"PRIu64"s)\n"
    - Duration target: CALIBRATE_DELAY_MS / 1000 seconds (= 10).
    - Exit the loop early if g_calibrated_threads >= n_threads OR g_progress_done.
    - Loop body: compute elapsed since progress_main start; sleep(1) between steps.
    - After the loop: print a blank "\n" line separator before the run bar.

  Phase 2 — Wait (existing, now very short):
    - Keep the existing g_calibrated_threads busy-poll so that if Phase 1 exits
      early (e.g. fast calibration), we still don't start the run bar prematurely.

  Phase 3 — Run bar (existing, unchanged):
    - \r-based bar from bar_start to stop_at.

  CALIBRATE_DELAY_MS is already visible in wrk.h via the thread, no new arg needed.

Argument change: none. progress_main still receives &stop_at only.

Changes required:
  - src/wrk.c: progress_main() — add Phase 1 loop before the existing wait.
  - No changes to Makefile, headers, or other files.

Acceptance:
- `make clean && make && make test` exits 0
- Running `./wrkx -t2 -c10 -d15s -R50 http://localhost:18080/` (after starting
  mock server in instant mode) shows the calibration bar scrolling for ~10s
  followed by the run bar for ~5s.
- Running with -d5s (shorter than CALIBRATE_DELAY_MS) exits cleanly mid-bar
  with no garbled output.
- make test-asan exits 0 (no memory errors introduced).
