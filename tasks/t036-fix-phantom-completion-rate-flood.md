title: Fix phantom-completion rate flood on Connection: close servers
status: todo
adr: 0001
adr-step: P1-3
depends: t027, t032

## Context

User report: `./wrkx -c28 -t27 -d20 -l -R3000 http://localhost` produced
`Requests/sec: 23744` (≈8× the −R3000 target) with multi-second latencies
(p50 3.29s) and bimodal calibration (some threads ~2ms, most ~350ms). The
original wrk.c (phase 0) gave correct values on the same server.

## Root cause

Reproduced by pointing both the phase-0 `wrk.c` binary and the phase-1 `wrkx`
at a `Connection: close` mock server (reconnect after every request):

- phase 0: ~1300 req/s, sane latency (paced)
- phase 1: ~12500 req/s, 11.47s latency (flood)

The bug is in `src/proto/http1.c::http1_readable`. When a response completes,
`s->complete` is set true (by `on_message_complete`) but **never consumed after
being reported**. The orchestrator keeps the readable event armed across the
request/response cycle. On a `Connection: close` server the response is
immediately followed by a FIN, so a second readable event fires on the EOF.
`http1_readable` reads 0 bytes, then checks `if (s->complete)` — still true from
the previous response — and returns `PROTO_DONE` **again**: a phantom
completion. Because the EOF is level-triggered, this re-enters in a tight loop,
each iteration:
  - double-counting a completion (`rate_expected_latency` → `complete++`), which
    corrupts the open-model pacing math, and
  - re-arming the writer.

The request count explodes while almost no bytes move, throughput runs ~10×
over `-R`, and CO-corrected latency balloons to seconds.

wrk.c cannot hit this: its `response_complete` is an http_parser callback fired
exactly once per actual message, and it reconnects immediately on
`!http_should_keep_alive` rather than reusing a closed socket.

## Fix

In `http1_readable`, consume the completion before returning: reset
`s->complete = false`, reinitialise the parser for the next response, and zero
`s->bytes`. The completion is then a one-shot. If the peer actually closed, the
next read returns EOF → `PROTO_ERROR` → the orchestrator reconnects (matching
phase-0 semantics). Keep-alive servers are unaffected (the next response parses
on the freshly-initialised parser).

## Tests added

- `tests/unit/test_http1.c::test_completion_then_close_reports_done_once` —
  feeds a complete response followed by a peer close; asserts the first
  readable returns `PROTO_DONE` and the second returns neither `PROTO_DONE` nor
  `PROTO_DONE_STATUS_ERR` (no phantom re-report). Fails without the fix
  (`Expected 1 to be not equal to 1`).
- `tests/e2e/mock_server.py` — new `close` mode (200 OK + `Connection: close`,
  then close).
- `tests/e2e/rate_close.sh` — runs `-R2000` against the `close` server and
  asserts measured Requests/sec ≤ 1.5× target. Pre-fix measured ~42000 (21×).

## Acceptance

- `wrkx -R<N>` against a `Connection: close` server holds ≈N req/s, not ~10×.
- `make test` green (unit + E2E), including the two new regression tests.
- Phase-1 and phase-0 throughput agree (±reconnect overhead) on close, keep-
  alive, flaky, and drop servers.
