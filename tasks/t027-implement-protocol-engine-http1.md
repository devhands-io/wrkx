title: Implement the Protocol Engine — transport + HTTP/1.1 (P1-3)
status: todo
adr: 0001
adr-step: P1-3
depends: t025

## Context

Implements the Protocol Engine layer (the "Machine Gun") behind `src/proto/proto.h`
(from t025). It knows nothing about rate control or scripting: it selects transport,
encodes/sends requests, buffers and decodes responses, and signals completion. See
ADR 0001 §"Layer responsibilities" and §"Phase 1 Migration Map".

Runs in parallel with t026 and t028 once t025 is done. Ships the first concrete
protocol (HTTP/1.1), proving the vtable is adequate.

## Scope (Migration-Map symbols this task owns)

- **`src/transport.c` (+ `transport.h`):** TCP / TLS, selectable and protocol-
  independent. Migrates `connect_socket`, `reconnect_socket`,
  `delayed_initial_connect`, `socket_connected`. Backs `proto->connect`.
- **`src/proto/http1.c`:** implements the `protocol` vtable for HTTP/1.1.
  - `proto->readable` ← `header_field`, `header_value`, `response_body`,
    `response_complete`, `parser_settings`, `sock` (http_parser callbacks +
    completion detection). Returns `PROTO_DONE | PROTO_PENDING | PROTO_ERROR`.
  - `proto->connect` performs connect (+ any handshake), allocates `proto_state`.
  - `proto->write` encodes and sends a request buffer owned by the Request Layer.
  - `proto->close` frees `proto_state`, closes the socket.

## Steps

- Define the HTTP/1.1 `proto_state` (parser, read buffer, completion flags).
- Wire the existing http_parser callbacks into `readable`, returning the tri-state.
- Keep transport selection (TCP vs TLS) behind `transport.c`, independent of http1.
- Add a unit test that feeds **raw bytes** to http1 `readable` (partial response →
  `PENDING`; complete → `DONE`; malformed → `ERROR`) with no running engine.

## Acceptance

- http1 `readable` returns the correct `proto_status` for partial / complete /
  malformed byte streams fed directly in a unit test (no network, no engine).
- **Invariant 2 holds:** no `proto/*.c` includes `lua.h`, `quickjs.h`, or any engine
  header. `grep -r 'lua\.h\|quickjs\.h' src/proto/` is empty.
- HTTP/1.1 behaviour is identical to pre-refactor (verified end-to-end in t029).
- `make test` green.
