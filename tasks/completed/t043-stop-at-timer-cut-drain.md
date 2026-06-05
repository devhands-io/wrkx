title: Cut post-stop_at drain — wake the event loop at stop_at (exact rate)
status: completed
adr: 0003
depends: t041

## Context

After t041 the refactored `wrkx` reports the requested rate a hair LOW
(`-R3000` → ~2999.7) where the frozen baseline reports ~3000.1. Investigation
(see below) showed request COUNTS are identical (~60022); the entire ~0.2 rps
gap is in the `runtime_us` denominator.

## Root cause — the ~10ms drain past stop_at

The open-model pacer schedules each connection's next send as a millisecond
TIME event (`delay_request` → `aeCreateTimeEvent`). At `-R3000 / 27 threads / 1
conn` the per-connection rate is ~111 req/s → an inter-send interval of ~9ms.
When the run reaches `stop_at`, every event loop is typically asleep in the
poller waiting for its next send timer; it wakes ~one interval later (~9ms),
sends, gets the response, and only then does the completion path observe
`now >= stop_at` and call `aeStop`. So `runtime_us` absorbs ~9-10ms of dead time
after `stop_at` in which no load was offered.

`check_timeouts` also calls `aeStop` past `stop_at`, but it runs only every
`TIMEOUT_INTERVAL_MS` (2000ms), so it never trips first — the ~9ms send-timer
path always wins.

This ~10ms drain is present in BOTH binaries. The baseline appears exact only
incidentally: it creates a Lua state per thread (~1.5-2.5ms ×27) between the
`stop_at` anchor and the measurement-`start` anchor, capturing `start` ~1.5ms
later and shortening its window enough to offset a bit more of the drain. The
refactor's leaner shared-engine setup removed that accidental offset, exposing
the true ~2999.7.

## Decision

Don't mimic baseline's accidental setup cost. Eliminate the dead time directly:
register a one-shot TIME event per worker scheduled at `stop_at` that calls
`aeStop(loop)`. The loop then wakes at `stop_at` (within ms granularity) and
stops promptly instead of sleeping a full send interval. Connections are idle
(between sends) at `stop_at`, so essentially no in-flight responses are lost,
and `runtime_us` ≈ the true offered-load window.

This is more correct than the status quo (it removes ~0.05% of dead time from
the denominator) and brings wrkx in line with the baseline's exact rate.

## Implementation

- `stop_event()`: one-shot timer callback → `aeStop(loop)`, returns AE_NOMORE.
- `thread_main()`: after the connect/calibrate/timeout timers, schedule
  `stop_event` at `(t->stop_at - now)/1000` ms.
- Existing `now >= stop_at` guards in `record_response`/`check_timeouts` stay as
  belt-and-suspenders.

## Acceptance (met)

- `wrkx -c28 -t27 -d20 -R3000 http://localhost` now reports `requests in
  20.00s` (was 20.01s — the ~10ms drain is gone) and Requests/sec that tracks
  `baseline/wrkx0` within run-to-run noise (both straddle 3000; e.g. NEW
  2999.6/2999.8/2999.9 vs OLD 2999.4/3000.1/2999.98). The previous systematic
  "NEW always below, OLD always above" bias is eliminated.
- NEW completes ~20 fewer responses than OLD (the post-stop_at in-flight ones
  OLD still drains) — expected and correct; those responses arrived after the
  measured window closed.
- `make test` and `make parity` green.
