title: Fix Transfer/sec always 0.00B — surface response bytes from protocol to orchestrator
status: completed
adr: 0001
depends: t032

## Symptom

```
./wrkx -c28 -t27 -d20 -L -R3000 http://localhost
...
Transfer/sec:       0.00B
```

`Requests/sec`, latency, and errors are all correct, but `Transfer/sec` (and the
`N.NNB read` figure on the summary line) are always `0.00B`, regardless of
response size.

## Root cause — no byte channel across the layer boundary

The orchestrator reports throughput from a per-thread accumulator `t->bytes`:

```c
bytes += t->bytes;                    // aggregate across threads
...
format_binary(runtime_s > 0 ? bytes / runtime_s : 0)   // Transfer/sec
```

But **nothing ever wrote `t->bytes`** — it stayed 0 for the whole run.

The Protocol Engine (`src/proto/http1.c`) does count response bytes internally
(`s->bytes += n` per read), but at completion it reset `s->bytes = 0` and
returned only a `proto_status`. The Phase-1 contract (`proto.h`) deliberately
had no channel to pass the byte count back — the http1.c design note even
recorded the decision:

> "PROTO_DONE/PENDING/ERROR is the only response signal; ... no extra return
> channel is needed in Phase 1."

That decision was wrong: the byte count was needed for `Transfer/sec`, so the
figure silently read zero. Phase-0 wrk.c had no boundary here — it did
`c->thread->bytes += n` directly in the read callback — so the regression was
introduced by the three-layer split (t026/t027/t032).

## Fix — minimal contract amendment (one word in `struct connection`)

Add a `size_t bytes` field to the shared `struct connection` in `proto.h` as a
one-shot response-byte channel:

- **proto.h**: `struct connection` gains `size_t bytes;` documented as "the wire
  size of the response the protocol just completed; set on every PROTO_DONE*
  return, read+accumulated by the orchestrator."
- **proto/http1.c**: at completion, before resetting the per-response counter,
  `c->bytes = s->bytes;` — set for all three PROTO_DONE* results so status-error
  and Connection:close completions are counted too (matches phase-0, which
  counts all bytes read regardless of status).
- **orchestrator.c**: in `record_response()`, `t->bytes += c->conn.bytes;` — the
  single place every completion is already tallied.

This closes the gap the http1.c design note flagged; the note is updated to
record that the channel now exists. HTTP status is still not surfaced (the
orchestrator asks the Request Layer for that), so no further contract change.

## Verification

- New unit test `test_done_surfaces_response_bytes` (tests/unit/test_http1.c):
  feeds a known-size response, asserts `conn.bytes` equals the wire size after
  PROTO_DONE.
- New e2e check R6 (tests/e2e/cli_output.sh): asserts `Transfer/sec` and the
  `... read` summary are both non-zero.
- Manual, mock server: `6000 requests in 3.00s, 375.00KB read` /
  `Transfer/sec: 124.93KB` (was 0.00B).
- Manual, real nginx (`-c28 -t27 -d5 -L -R3000 http://localhost`):
  `12.23MB read`, `Transfer/sec: 2.44MB`, `Requests/sec 3003.29`.
- `make test` green (12 cli_output checks, http1 byte test).

## Acceptance (met)

- `Transfer/sec` and `... read` are non-zero and proportional to response size.
- Bytes counted for 2xx, non-2xx, and Connection:close completions alike.
- No regression in rps / latency / error accounting; full suite green.
