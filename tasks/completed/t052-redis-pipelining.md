title: Redis pipelining — multiple in-flight operations, Gate B confirmation
status: todo
adr: 0005
adr-step: P2-4
depends: t051

## Goal

Implement Redis command pipelining: multiple commands sent without waiting for each
reply, with correct per-command latency attribution and response correlation. This
proves Gate B: the engine can account for multiple outstanding logical operations
on one physical connection.

## Context

ADR 0005 P2-4. Basic Redis (t049–t051) proves "non-HTTP request/response protocols
work." Pipelining proves the stronger claim: the scheduler, rate controller, and
latency machinery survive multiple in-flight logical requests per connection.

This is the cheapest proof of the Gate B claim — the same family of problem as
HTTP/2 stream multiplexing but with a far simpler framing protocol. Discovering a
Gate B failure here costs one subphase; discovering it at HTTP/2 (Phase 8) costs
a full phase of multiplexing work.

## Deliverables

- **Pipeline depth configuration:** `--pipeline N` CLI flag (default 1 = disabled);
  protocol-level: buffer N commands, flush, read N replies before the next send
- **Response correlation:** replies arrive in command-issue order (RESP guarantees
  this); the vtable must surface N completions from one `readable` call
- **Per-command latency attribution:** each command in the pipeline gets its own
  latency sample (issue time → reply time for *that command*, not the batch)
- **Backpressure:** if the pipeline is full (N commands outstanding, zero replies
  received) the connection pauses sending until at least one reply is consumed
- **Lua API:** `redis.pipeline({ "SET k v", "GET k", "INCR counter" })` — replaces
  the stub from t050 with a working implementation

## Guards / Acceptance

1. **Pipelined E2E test** — `tests/e2e/redis_pipeline.sh`:
   - `--pipeline 10` against real Redis at 1000 RPS
   - throughput ≥ single-command throughput (pipelining must not regress RPS)
   - per-command error count reported correctly (not per-batch)
2. **Latency correctness test:**
   - pipeline depth 1 and pipeline depth 10 with artificial server delay:
     P50 latency at depth 10 must be ≥ P50 at depth 1 (pipelining doesn't hide
     server latency) but total throughput at depth 10 must be higher
   - per-command latency samples are present in the histogram (not one sample
     per pipeline flush)
3. **Backpressure test:**
   - saturate a slow Redis (via `DEBUG SLEEP`) with pipeline depth 50;
     confirm wrkx does not buffer unbounded commands or OOM
4. **Gate B check:**
   - `git diff origin/main -- src/orchestrator.c src/rate.c src/ae.c` produces
     zero scheduler-semantic changes, OR any changes are explicitly listed in a
     "Accepted core changes" section appended below this line
   - If scheduler changes are required, open a follow-up ADR before merging

## Accepted core changes

*(Leave blank. If Gate B fails and core changes are required, document them here
and open a revision to ADR 0005 before proceeding to Phase 3.)*

## Note on HTTP/2 relationship

Redis pipelining and HTTP/2 multiplexing are in the same problem family (multiple
in-flight logical operations per connection) but differ in ordering guarantees:
- RESP pipelines are strictly ordered (replies in command order)
- HTTP/2 streams are unordered (HEADERS frames carry stream IDs for correlation)

The concurrency-model review in t??? (P8-1) should explicitly document how the
vtable abstraction chosen for pipelining here extends (or is intentionally
different) for HTTP/2 streams.
