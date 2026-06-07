title: memcached text codec — encode commands and parse replies
status: completed
adr: 0005
adr-step: P4-1
depends: t058

## Goal

Implement standalone memcached text-protocol encode/decode helpers for the first
supported command set: `get`, `set`, `delete`, `incr`, and `decr`.

## Context

Keep framing and parser work separate from networking so failures are local and
cheap to diagnose. This task should not open sockets or depend on the scheduler.

## Deliverables

- command encoders for `get`, `set`, `delete`, `incr`, `decr`
- response parser for value replies, stored/not stored, deleted/not found,
  counter values, client errors, and server errors
- parser state that handles partial reads and multiple responses in one buffer
- fixtures for representative success and failure replies

## Guards / Acceptance

1. Unit tests cover complete replies, partial replies, and coalesced replies.
2. Unit tests cover `VALUE ... END`, `STORED`, `NOT_STORED`, `DELETED`,
   `NOT_FOUND`, numeric counter replies, `CLIENT_ERROR`, and `SERVER_ERROR`.
3. Codec code remains private to `extensions/memcached/`.
4. No network or E2E behavior is introduced in this task.

