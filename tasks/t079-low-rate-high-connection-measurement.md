title: investigate: -R1 -c400 gives 4 measurement requests instead of ~18
status: open

## Observed

```
./wrkx -c400 -t4 -d18 -R1 http://localhost
  Measurement: 4 requests in 18.00s, 3.33KB read
  Total:       400 requests in 18.50s, 333.20KB read (+ 495.03ms ramp-up, 396 req excluded)
Requests/sec:      0.22
```

Expected ~18 requests (1 req/s × 18s). Actual: 4.

## Root cause analysis

### What the 400 total requests are

Every connection sends exactly one request immediately on establishment — this is
the first call to `socket_writeable` after `oc_connect` registers the writable
event. At that point `rc->complete == 0`, so `rate_usec_to_next_send` returns 0
(send now). All 400 connections do this during the ramp-up phase.

### Why 4 leak into the measurement window (race)

`signal_connection_ready` is called inside `delayed_initial_connect` on `oc_connect`
success, *before* the first request is sent or received. The sequence for the last
few connections is:

```
connection N establishes
  → signal_connection_ready() sets connections_ready_at        ← window opens here
  → AE writable event fires → first request sent
  → response received → record_response: measuring=true        ← counted as measurement
```

The first request for those connections was sent after `connections_ready_at` was
set, so it is classified as a measurement request rather than ramp-up. With 4
threads the last few connections per thread land in this window, giving ~4 leaked
first-requests.

### Why no further requests fire (structural)

Per-connection throughput:

```
throughput_per_conn = (rate / threads) / per_thread_conns / 1_000_000
                    = (1 / 4) / 100 / 1_000_000
                    = 2.5e-9 req/µs
```

After the first request `rc->complete = 1`. Next send time:

```
next_start_time = thread_start + rc->complete / throughput
                = thread_start + 1 / 2.5e-9
                = thread_start + 400_000_000 µs   (400 seconds)
```

No connection fires a second time within the 18s measurement window. Expected
combined rate of 1 req/s across 400 connections means one connection fires every
400s — none within 18s.

## Two separate problems to fix

### Problem 1 — Race: first-requests leaking into the measurement window

`signal_connection_ready` fires before the first request is sent/received.
Fix candidate: move the ready signal to after the first *response* for each
connection rather than after the TCP connect. This ensures `connections_ready_at`
is set only after every connection has completed its initialization request, so
all 400 first-requests land in the ramp-up bucket.

Implementation sketch: add `bool initial_response_done` to `oconn`; in
`record_response`, if `!c->initial_response_done`, set it and call
`signal_connection_ready(t)` instead of doing so in `delayed_initial_connect`.
Remove the `signal_connection_ready` call from `delayed_initial_connect`.

### Problem 2 — Structural: per-connection inter-request time >> test duration

With 400 connections at 1 req/s the per-connection rate is 1/400 req/s (400s
between requests). After the initial burst no second request fires within any
reasonable test duration. This is a misconfiguration, but the tool silently
produces nonsensical output (4 requests / 0.22 req/s) instead of warning.

Fix candidates:
- **Warn at startup** if `(rate / connections) < some_threshold` or if
  `1 / (rate / connections)` (inter-request interval per connection) exceeds
  `duration_us`. E.g.: "Warning: at -R1 with 400 connections each connection
  fires once every 400s; only the initial burst will be measured in a 18s test."
- **Clamp active connections to rate**: open `min(connections, ceil(rate))` 
  connections and use the rest as idle reserve. Simpler than a full shared-pool
  scheduler.
- **Shared-pool scheduling**: decouple rate from connection identity — maintain
  a global token bucket and let any idle connection pick up the next token. This
  is the correct long-term fix but requires replacing the per-connection rate
  controller with a thread-level or global one.

## Questions to answer during investigation

1. Is the race (Problem 1) always 4, or does it vary by run / platform?
2. What is the minimal fix for Problem 1 that doesn't break normal operation?
3. For Problem 2, is the warn-only approach sufficient or does the scheduling
   model need to change?
4. Does `-R0` or very small rates (< 1/connections) need special-casing?
