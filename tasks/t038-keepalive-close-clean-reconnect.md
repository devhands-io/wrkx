title: Handle server keep-alive close cleanly (PROTO_DONE_CLOSE) — stop spurious read errors
status: todo
adr: 0003
adr-step: B
depends: t027, t036

## Context

ADR 0003 Defect B. Against a real keep-alive server (nginx, default
`keepalive_requests 1000`), the new `wrkx` reports spurious `read` socket errors
(≈ total_requests ÷ 1000: 27 at 10s, 54 at 20s) and a fat latency tail, where
phase-0 reports zero. nginx closes each connection after N responses, sending
`Connection: close` on the last. Phase-0 `response_complete` checks
`http_should_keep_alive` and reconnects cleanly. The new `http1_readable`
records `s->keep_alive` (via `on_message_complete`) but never propagates it —
it returns `PROTO_DONE`, the orchestrator re-arms a closing socket, and the
following EOF becomes `PROTO_ERROR` → `errors.read++` + reconnect churn.

Root contract gap: `proto_status` has no value for "response complete AND peer
is closing — reconnect, not an error." Same keep-alive-state loss that caused
the t036 phantom-completion flood. See ADR 0003 §"Decision B".

## Scope

- `src/proto/proto.h` — additive enum value (no struct change).
- `src/proto/http1.c` — return the new value when `!keep_alive`.
- `src/orchestrator.c` — handle the new value: count + record the response, then
  reconnect WITHOUT incrementing `errors.read`.

## Steps

1. `proto/proto.h`: add `PROTO_DONE_CLOSE` to `proto_status` (between
   `PROTO_DONE_STATUS_ERR` and `PROTO_ERROR`), documented as "response complete;
   peer closing — reconnect cleanly".
2. `http1.c::http1_readable`: when a response completes, if `!s->keep_alive`
   return `PROTO_DONE_CLOSE` (non-2xx `PROTO_DONE_STATUS_ERR` still takes
   priority for the error counter as today). Keep the t036 completion-consume
   (reset `s->complete`, reinit parser) intact.
3. `orchestrator.c::socket_readable`: add a `case PROTO_DONE_CLOSE:` that calls
   `complete_response()` (so the response is counted, latency recorded, script
   hook fired) and then performs a clean `oc_reconnect()` **without**
   `t->errors.read++`. (Mind interaction with the existing `complete_response`
   re-arm: ensure exactly one reconnect, no double-arm.)

## Acceptance

- Against a keepalive-limited static server, `wrkx` reports **0 read errors**
  (matching phase-0) for a run that previously showed ~54.
- Latency tail matches phase-0 (no reconnect-induced p99.9 spike).
- New unit test in `tests/unit/test_http1.c`: a completed response carrying
  `Connection: close` returns `PROTO_DONE_CLOSE` (not `PROTO_DONE`/`PROTO_ERROR`);
  fails before the fix.
- `make test` green; `make adr-check` green; `baseline-verify` green.
