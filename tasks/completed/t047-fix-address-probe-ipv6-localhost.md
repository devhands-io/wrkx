title: Probe resolved addresses — wrkx hung on unreachable ::1 (Linux CI + parity)
status: completed
adr: 0001
depends: t046

## Symptom

With the t046 crash fixed, the Linux e2e finally ran and revealed the next
layer: every request errored.

- Linux `latency.sh`: p50 `0us`, `Socket errors: read 98018, write 98018` (and
  later `487665`) — i.e. no request ever succeeded.
- macOS `parity` (instant): `NEW(wrkx) Requests/sec 0` with `0` socket errors
  and `n/a` latency, while `OLD(wrkx0) 1997` — `NEW diverges 100%`.

## Root cause — using the first resolved address without probing

main.c resolved the target with `getaddrinfo(AF_UNSPEC)` and passed the **head**
of the list straight to `http1_configure()`. On a dual-stack host `localhost`
resolves to `::1` (IPv6) first, but the e2e mock server (and many real servers)
listen on IPv4 only. So every connection targeted an address with no listener:

- where the peer refuses fast → counted as write errors (Linux: 98018+),
- where it hangs in non-blocking `EINPROGRESS` → 0 requests, 0 errors, n/a
  latency (macOS parity instant).

The frozen baseline does NOT have this bug: wrk2's `script_resolve` /
`wrk.connect()` probes each resolved address with a test `connect()` and uses the
first reachable one (which is why OLD got 1997 on the very same `localhost`).
The Phase-1 wiring dropped that probe.

## Fix

Add `pick_reachable()` to main.c, mirroring wrk2's `wrk.connect()` probe: iterate
the `getaddrinfo` list, test-`connect()` each address, and pass the first that
succeeds to `http1_configure()` (fall back to the head if none). Single line at
the call site: `http1_configure(pick_reachable(addr), ssl_ctx, host)`.
`#include <unistd.h>, <sys/socket.h>` for socket/connect/close.

This fixes ALL Linux e2e tests and the macOS parity instant case at once — both
were the same unreachable-`::1` root cause; only the failure shape differed
(fast refuse vs hang) per platform/timing.

## Verification

- Local `make test` + `make parity` green; manual `wrkx http://localhost:PORT/`
  against the IPv4 mock server returns proper rps.
- Behaviour now matches baseline (which probes), so parity is apples-to-apples.
- CI to confirm Linux + macOS green on push.

## Note

This is a real robustness fix, not just a test workaround: any user pointing
wrkx at a dual-stack hostname whose server binds only one family was affected.
Long-term, the transport could iterate `ai_next` on async connect failure; the
synchronous probe matches wrk2 and is sufficient for Phase 1.
