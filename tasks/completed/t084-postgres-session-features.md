title: postgres extension — P6-3 session features (TLS, SCRAM, result decoding, transactions, QuickJS)
status: completed
adr: 0005
adr-step: P6-3
depends: t083

## Why / Goal

P6-3 closes the remaining gaps deferred from P6-1 and P6-2:

- **TLS** — PostgreSQL's SSLRequest prelude requires a custom handshake step before
  the TLS negotiation; plain `transport_connect` is insufficient for real servers.
- **SCRAM-SHA-256** — modern PostgreSQL servers default to SCRAM; MD5-only support
  excludes most production targets.
- **Result-set decoding** — `DataRow` values have been counted but never decoded;
  without column names and field values the `response()` hook is useless for
  assertions or adaptive scripting.
- **Multi-query pipelining** — `pg.begin() .. pg.execute() .. pg.commit()` sends
  multiple server messages in one `write()` call; `readable()` must wait for all
  `ReadyForQuery` replies, not just the first.
- **QuickJS parity** — `pg@lua` has no QuickJS counterpart, unlike every other
  extension.

Gate E is already confirmed (P6-2).  P6-3 is a quality-and-completeness step: no
new gates, no core engine changes.

---

## Deliverables

### 1. `extensions/postgres/pg_message.c/h` — codec additions

#### 1a. Extended `pg_parsed_msg` for result decoding

Grow `pg_parsed_msg` to carry decoded column descriptors from `RowDescription`
and decoded field values from `DataRow`.  Field value pointers into the parse
buffer are valid only for the duration of the `pg_parse_message()` call; callers
must copy before the buffer shifts.

```c
#define PG_RESULT_MAX_COLS  64

/* Column descriptor from RowDescription — store in pg_state.cols[] */
typedef struct {
    char    name[64];     /* column name, truncated if longer */
    int32_t type_oid;     /* raw OID for future type coercion */
} pg_col_desc_t;

/* In pg_parsed_msg, extend row_description and data_row: */

struct {
    int16_t      ncols;
    pg_col_desc_t cols[PG_RESULT_MAX_COLS];
} row_description;

pg_data_row_t data_row;
```

`pg_data_row_t` is a **named type** defined in `pg_message.h` so that
`pg_lua_helpers.c` can reference it without casting through `void *` or
duplicating the layout:

```c
/* In pg_message.h, before pg_parsed_msg: */
typedef struct {
    int16_t  nfields;
    struct {
        int32_t     len;      /* -1 for SQL NULL */
        const char *data;     /* points into parse buf; copy before buf shifts */
    } fields[PG_RESULT_MAX_COLS];
} pg_data_row_t;
```

`pg_result.h` (which includes `pg_message.h`) then uses
`const pg_data_row_t *` throughout instead of `const void *`.

`pg_parse_message` already handles `RowDescription` and `DataRow` from P6-1; extend
both parsers to fill in the new fields.

For `RowDescription`: each column descriptor is `name\0 + int32(table_oid) +
int16(attnum) + int32(type_oid) + int16(type_len) + int32(type_modifier) +
int16(format_code)`.  Extract `name` (truncate to 63 chars) and `type_oid`;
skip the remaining fields.

For `DataRow`: `int16(nfields)` followed by `nfields × (int32(len) + len bytes)`.
Store each `(len, data)` pair in `msg.data_row.fields[i]`.  If `len == -1` the
field is SQL NULL; leave `data = NULL`.  Stop at `PG_RESULT_MAX_COLS`; additional
fields are consumed but not stored.

#### 1b. New message types for SCRAM

```c
/* Add to pg_msg_type enum: */
PG_MSG_AUTH_SASL_CONTINUE,   /* R, authtype=11 — server-first-message    */
PG_MSG_AUTH_SASL_FINAL,      /* R, authtype=12 — server-final-message    */
```

Extend `pg_parsed_msg` union, and **replace** the P6-1 `sasl` member
(`char sasl_mechanisms[128]` — first mechanism only) with a full-list version:

```c
/* Replace P6-1's { char sasl_mechanisms[128]; } sasl with: */
struct { char list[256]; size_t len; } sasl;           /* full NUL-separated list */

struct { char data[512]; size_t len; } sasl_continue;  /* server-first-message */
struct { char data[256]; size_t len; } sasl_final;     /* server-final-message */
```

`sasl.list` receives the raw bytes of the `AuthenticationSASL` payload — the
sequence of NUL-terminated mechanism name strings, double-NUL terminated by the
server.  `sasl.len` is the number of bytes written into `sasl.list` (capped at
255 to leave room for a final NUL; truncation is safe because mechanism names
beyond the buffer are simply unreachable and the fallback error path handles the
case where no supported mechanism is found).

Parse `AuthenticationSASLContinue` and `AuthenticationSASLFinal` by checking the
`authtype` field in `R` messages (already split from `AUTH_OK` / `AUTH_MD5` etc.).

#### 1c. New encode functions for SASL responses

```c
/* 'p' + int32_len + mechanism + '\0' + int32(cf_len) + client_first */
int pg_encode_sasl_initial_response(char *buf, size_t cap,
                                    const char *mechanism,
                                    const char *client_first, size_t cf_len);

/* 'p' + int32_len + client_final (no null terminator per SASL wire format) */
int pg_encode_sasl_response(char *buf, size_t cap,
                            const char *client_final, size_t cf_len);
```

Both return bytes written or ≤ 0 on buffer-too-small.

#### 1d. `ReadyForQuery` status byte

The existing `PG_MSG_READY_FOR_QUERY` parse discards the status byte.  P6-3 needs
it for connection reset detection:

```c
/* Extend pg_parsed_msg: */
struct { uint8_t status; } rfq;   /* 'I'=idle, 'T'=in transaction, 'E'=error */
```

---

### 2. `extensions/postgres/pg_scram.c` + `pg_scram.h` — SCRAM-SHA-256

SCRAM-SHA-256 follows RFC 5802.  Uses OpenSSL primitives already linked
(`EVP_MD_CTX`, `EVP_sha256`, `HMAC`); no new dependencies.

```c
/*
 * Generate the client-first-message-bare: "n=<username>,r=<nonce>".
 * The GS2 header ("n,,") is NOT included — the caller must prepend it when
 * building the SASLInitialResponse payload sent to the server, and omit it
 * when computing AuthMessage.
 * nonce_out receives only the nonce token (for use in pg_scram_client_final).
 * Returns bytes written into buf, or -1 on buffer-too-small.
 */
int pg_scram_client_first(const char *username,
                          char nonce_out[64],
                          char *buf, size_t cap);

/*
 * Compute the client-final-message.
 * server_first:        the data field from PG_MSG_AUTH_SASL_CONTINUE.
 * client_nonce:        the nonce from pg_scram_client_first (bare token, no "n,,").
 * client_first_bare:   the buf returned by pg_scram_client_first (for AuthMessage).
 * client_first_bare_len: its length.
 * password:            cleartext password.
 * expected_server_sig_out: receives HMAC(ServerKey, AuthMessage) — 32 bytes.
 *                          Pass this to pg_scram_verify_server.
 * Returns bytes written into buf (the client-final-message), or -1 on error.
 */
int pg_scram_client_final(const char *server_first, size_t sf_len,
                          const char *client_nonce,
                          const char *client_first_bare, size_t cf_bare_len,
                          const char *password,
                          uint8_t expected_server_sig_out[32],
                          char *buf, size_t cap);

/*
 * Verify the server-final-message.
 * server_final:         the data field from PG_MSG_AUTH_SASL_FINAL.
 * expected_server_sig:  the 32-byte HMAC(ServerKey, AuthMessage) from
 *                       pg_scram_client_final — NOT the raw ServerKey.
 * Base64-decodes the "v=" field and compares it byte-for-byte.
 * Returns 1 if valid, 0 otherwise.
 */
int pg_scram_verify_server(const char *server_final, size_t sf_len,
                           const uint8_t expected_server_sig[32]);
```

**Internal SCRAM steps** (implemented inside `pg_scram.c`):

```
SaltedPassword = Hi(password, salt, i)           — PBKDF2-HMAC-SHA256
ClientKey      = HMAC(SaltedPassword, "Client Key")
StoredKey      = SHA256(ClientKey)
AuthMessage    = client-first-bare + "," + server-first + "," + client-final-no-proof
ClientSignature= HMAC(StoredKey, AuthMessage)
ClientProof    = ClientKey XOR ClientSignature
ServerKey      = HMAC(SaltedPassword, "Server Key")
ServerSignature= HMAC(ServerKey, AuthMessage)
```

Base64 encode/decode using OpenSSL's `EVP_EncodeBlock` / `EVP_DecodeBlock`.

PBKDF2 via `PKCS5_PBKDF2_HMAC(password, passlen, salt, saltlen, iterations,
EVP_sha256(), 32, out)`.

Parse the `server-first-message` for `r=` (combined nonce), `s=` (base64 salt),
`i=` (iteration count).  The combined nonce must start with `client_nonce`;
fail if it does not.

**Username escaping (RFC 5802 §5.1):** the `n=` attribute must escape two
characters before writing into the `client-first-message-bare`:

```
',' → "=2C"
'=' → "=3D"
```

`pg_scram_client_first` must apply this substitution to `username` before
writing `n=<escaped>,r=<nonce>`.  PostgreSQL role names can legally contain
both characters.  Failure to escape produces a malformed SCRAM attribute string
that the server will reject.

---

### 3. `extensions/postgres/postgres.c` — connect/write/readable updates

Add `#include <errno.h>` and `#include <poll.h>` to `postgres.c`'s include
block; both are required by the TLS prelude code added in this task.

#### 3a. TLS: SSLRequest prelude

PostgreSQL TLS requires an 8-byte `SSLRequest` message before the TLS handshake.
The current `transport_connect` path assumes the TLS negotiation starts
immediately after TCP; it cannot be used directly for PostgreSQL TLS.

The extension owns its embedded `transport xport` and can manipulate its fields
directly.  The prelude logic lives entirely inside `postgres_connect`:

```c
static int postgres_connect(connection *c) {
    pg_state *s = calloc(1, sizeof(pg_state));
    ...

    bool use_tls = (g_cfg.ssl_ctx != NULL);

    if (use_tls) {
        /* Phase 1: TCP-only connect (ssl_ctx = NULL → no TLS yet).
         * transport_connect sets O_NONBLOCK and may return TRANSPORT_OK even
         * while the kernel connect() is still in progress (EINPROGRESS).
         * We must poll for POLLOUT and check SO_ERROR before proceeding. */
        transport_init(&s->xport, g_cfg.addr, NULL, g_cfg.host);
        if (transport_connect(&s->xport, &c->fd) != TRANSPORT_OK) goto fail;

        struct pollfd pfd = { .fd = s->xport.fd, .events = POLLOUT };

        /* Wait for non-blocking connect to complete */
        if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) goto fail;
        int so_err = 0; socklen_t so_len = sizeof(so_err);
        getsockopt(s->xport.fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
        if (so_err != 0) goto fail;

        /* Phase 2: SSLRequest prelude — send with EAGAIN/EWOULDBLOCK/EINTR retry.
         * EAGAIN and EWOULDBLOCK are the same value on Linux but distinct on
         * some POSIX platforms; check both.  EINTR means a signal interrupted
         * the syscall and the call should be reissued immediately.
         * Requires: #include <errno.h>  — add to postgres.c include block. */
#define PG_RETRY_ERRNO(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
        static const uint8_t ssl_request[8] = {
            0,0,0,8,              /* int32(8)        */
            0x04,0xd2,0x16,0x2f  /* int32(80877103) */
        };
        size_t sent = 0;
        while (sent < 8) {
            ssize_t n = send(s->xport.fd, ssl_request + sent, 8 - sent, 0);
            if (n > 0) { sent += (size_t)n; continue; }
            if (n < 0 && PG_RETRY_ERRNO(errno)) {
                if (errno != EINTR) {          /* EINTR: retry immediately */
                    pfd.events = POLLOUT;
                    if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) goto fail;
                }
                continue;
            }
            goto fail;
        }

        /* Read 1-byte SSL response with EAGAIN/EWOULDBLOCK/EINTR retry */
        uint8_t ssl_resp = 0;
        while (1) {
            ssize_t n = recv(s->xport.fd, &ssl_resp, 1, 0);
            if (n == 1) break;
            if (n < 0 && PG_RETRY_ERRNO(errno)) {
                if (errno != EINTR) {
                    pfd.events = POLLIN;
                    if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) goto fail;
                }
                continue;
            }
            goto fail;
        }
        if (ssl_resp != 'S') {
            fprintf(stderr, "postgres: server rejected SSLRequest (got '%c')\n",
                    (char)ssl_resp);
            goto fail;
        }

        /* Phase 3: manual TLS handshake with WANT_READ/WANT_WRITE loop.
         * The socket is still O_NONBLOCK, so SSL_connect returns -1 with
         * SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE until complete. */
        SSL *ssl = SSL_new(g_cfg.ssl_ctx);
        if (!ssl) goto fail;
        SSL_set_fd(ssl, s->xport.fd);
        if (g_cfg.host)
            SSL_set_tlsext_host_name(ssl, g_cfg.host);
        while (1) {
            int r = SSL_connect(ssl);
            if (r == 1) break;
            int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ) {
                pfd.events = POLLIN;
            } else if (err == SSL_ERROR_WANT_WRITE) {
                pfd.events = POLLOUT;
            } else {
                SSL_free(ssl);
                goto fail;
            }
            if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) {
                SSL_free(ssl);
                goto fail;
            }
        }

        /* Inject completed TLS object into transport */
        s->xport.ssl         = ssl;
        s->xport.ssl_ctx     = g_cfg.ssl_ctx;
        s->xport.handshaking = false;

    } else {
        transport_init(&s->xport, g_cfg.addr, NULL, g_cfg.host);
        if (transport_connect(&s->xport, &c->fd) != TRANSPORT_OK) goto fail;
    }

    /* Startup + auth handshake (unchanged from P6-1 except SCRAM branch added) */
    ...
}
```

`PG_AUTH_TIMEOUT_MS` (already defined in P6-1 as 10000) is reused for all
`poll()` calls in the TLS prelude.  The `connect()` vtable is always called
synchronously at connection setup (same thread, before the event loop starts),
so a poll-based retry loop here is correct and does not stall the event loop.

#### 3b. SCRAM auth in `connect()` startup loop

Extend the startup loop to handle the three SCRAM message types:

```
- AUTH_SASL        → scan the full NUL-separated list in msg.sasl.list / .len:
                       bool found_plain = false, found_plus = false;
                       const char *p = msg.sasl.list;
                       const char *end = p + msg.sasl.len;
                       while (p < end) {
                           if (!strcmp(p, "SCRAM-SHA-256"))      found_plain = true;
                           if (!strcmp(p, "SCRAM-SHA-256-PLUS")) found_plus  = true;
                           p += strlen(p) + 1;
                       }
                       if (!found_plain) {
                           if (found_plus)
                               fprintf(stderr, "postgres: server requires "
                                   "SCRAM-SHA-256-PLUS (channel binding); "
                                   "configure plain SCRAM or wait for PLUS support\n");
                           else
                               fprintf(stderr, "postgres: no supported SASL mechanism\n");
                           goto fail;
                       }
                       /* SCRAM-SHA-256 (plain) found — proceed */
                       int n = pg_scram_client_first(g_cfg.user,
                                   s->scram.nonce,
                                   s->scram.bare, sizeof(s->scram.bare));
                       if (n <= 0) goto fail;   /* buffer too small */
                       s->scram.bare_len = (size_t)n;
                       /* SASLInitialResponse payload = "n,," + bare */
                       char cf_full[512];
                       memcpy(cf_full, "n,,", 3);
                       memcpy(cf_full + 3, s->scram.bare, s->scram.bare_len);
                       pg_encode_sasl_initial_response(buf, cap, "SCRAM-SHA-256",
                                                       cf_full, 3 + s->scram.bare_len)
                       transport_write(...)
                     else: log error; goto fail
- AUTH_SASL_CONTINUE → if (!g_cfg.password) {
                           fprintf(stderr,
                               "postgres: SCRAM auth requires a password "
                               "(add :password to the URL)\n");
                           goto fail;
                       }
                       pg_scram_client_final(msg.sasl_continue.data, sf_len,
                                              s->scram.nonce,
                                              s->scram.bare, s->scram.bare_len,
                                              g_cfg.password,
                                              s->scram.expected_server_sig,
                                              final_buf, sizeof(final_buf))
                       pg_encode_sasl_response(buf, cap, final_buf, final_len)
                       transport_write(...)
- AUTH_SASL_FINAL   → pg_scram_verify_server(msg.sasl_final.data, sf_len,
                                              s->scram.expected_server_sig)
                       if !verify: log error; goto fail
                       (loop continues; AUTH_OK + ReadyForQuery follow)
```

Add to the `pg_state` struct for SCRAM transient state (valid only during `connect()`):

```c
struct {
    char    nonce[64];
    char    bare[256];              /* client-first-message-bare (no GS2 header) */
    size_t  bare_len;               /* byte length of bare; set before CONTINUE */
    uint8_t expected_server_sig[32];/* HMAC(ServerKey, AuthMessage) from client_final */
} scram;                            /* zeroed after AUTH_OK */
```

#### 3c. `pg_state` additions

```c
typedef struct pg_state {
    transport     xport;
    char          rbuf[PG_RECVBUF];
    size_t        rbuf_len;
    pg_phase      phase;
    bool          done;
    bool          error;
    size_t        bytes;
    int32_t       row_count;
    uint8_t       pg_status;      /* ReadyForQuery status: 'I', 'T', 'E'       */
    int32_t       queries_pending;/* Q + Sync messages sent; wait for all RFQ  */

    /* Column descriptors from RowDescription — reset on each new query */
    pg_col_desc_t cols[PG_RESULT_MAX_COLS];
    int16_t       ncols_desc;

    struct {                            /* SCRAM transient state — only in connect() */
        char    nonce[64];
        char    bare[256];              /* client-first-message-bare */
        size_t  bare_len;               /* byte length of bare */
        uint8_t expected_server_sig[32];/* for pg_scram_verify_server */
    } scram;
} pg_state;
```

#### 3d. Multi-query pipelining in `write()`

A script can concatenate multiple wire messages into one `request()` return value:

```lua
return pg.begin() .. pg.execute("INSERT INTO t VALUES($1)", id) .. pg.commit()
```

This produces one Q message + one P/B/E/S sequence + one Q message.  The server
sends three `ReadyForQuery` replies.  The current `readable()` returns
`PROTO_DONE` on the first `ReadyForQuery`; P6-3 must wait for all of them.

In `write()`, count the number of expected `ReadyForQuery` replies by scanning
the buffer for Q tags (`0x51`) and Sync tags (`0x53`):

```c
static int32_t count_rfq_expected(const char *buf, size_t len) {
    int32_t count = 0;
    size_t  pos   = 0;
    while (pos + 5 <= len) {
        uint8_t  tag = (uint8_t)buf[pos];
        uint32_t mlen;
        memcpy(&mlen, buf + pos + 1, 4);
        mlen = ntohl(mlen);
        if (tag == 'Q' || tag == 'S') count++;
        if (mlen < 4) break;               /* malformed; stop scanning */
        pos += 1 + mlen;
    }
    return count > 0 ? count : 1;         /* at least 1 for the simple case */
}
```

`queries_pending` is set in `write()` as part of the new-cycle guard described
in §3e (below).

In `readable()`, on `PG_MSG_READY_FOR_QUERY`:

```c
s->pg_status = msg.rfq.status;
if (--s->queries_pending <= 0) {
    s->done              = true;
    s->phase             = PG_PHASE_READY; /* re-arm for next write() cycle */
    tls_result.valid     = true;           /* mark available for pg.result() */
    tls_result.pg_status = s->pg_status;   /* 'I', 'T', or 'E' */
    consume;
    break;
}
consume;
continue;   /* more ReadyForQuery messages expected */
```

This is a backwards-compatible change: single-query scripts produce
`queries_pending = 1` and the behaviour is identical to P6-2.

#### 3e. Result decoding in `readable()`

Reset thread-local result and count expected `ReadyForQuery` replies at the
start of each new request cycle.  The orchestrator may call `proto->write()`
more than once if the first attempt is a partial write (see
`src/orchestrator.c` write loop); on a continuation call the buffer pointer
advances into the middle of the message stream.  Resetting or recounting on a
continuation call would make `readable()` wait for the wrong number of
`ReadyForQuery` messages.

Guard with `s->phase == PG_PHASE_READY` (i.e. this is a fresh request, not a
continuation):

```c
/* in write(): only on a new request cycle */
if (s->phase == PG_PHASE_READY) {
    pg_result_reset();
    s->ncols_desc      = 0;
    s->queries_pending = count_rfq_expected(buf, len);
    s->phase           = PG_PHASE_QUERY;
}
/* forward bytes regardless */
transport_write(&s->xport, buf, len, &nw);
```

In `readable()`:

```c
case PG_MSG_ROW_DESCRIPTION: {
    /* Clamp and store so pg_result_append_row() never exceeds the arrays */
    int16_t raw = msg.row_description.ncols;
    int16_t nc  = (raw > PG_RESULT_MAX_COLS) ? PG_RESULT_MAX_COLS : raw;
    s->ncols_desc = nc;   /* store clamped count; used by DATA_ROW branch */
    memcpy(s->cols, msg.row_description.cols, nc * sizeof(pg_col_desc_t));
    /* Also populate tls_result.cols/ncols now, so zero-row queries still
     * return correct column metadata from pg.result() */
    pg_result_set_columns(s->cols, nc);
    s->bytes += consumed;
    consume;
    continue;
}

case PG_MSG_DATA_ROW:
    pg_result_append_row(s->cols, s->ncols_desc,
                         &msg.data_row, consumed);  /* msg.data_row is pg_data_row_t */
    s->bytes  += consumed;
    s->row_count++;
    consume;
    continue;

case PG_MSG_COMMAND_COMPLETE:
    pg_result_set_cmd_tag(msg.cmd_complete.tag);
    s->bytes += consumed;
    consume;
    continue;
```

`pg_result_set_columns`, `pg_result_append_row`, `pg_result_reset`, and
`pg_result_set_cmd_tag` are implemented in `pg_lua_helpers.c` and declared in
`pg_result.h`; `postgres.c` includes `pg_result.h`.

---

### 4. `extensions/postgres/pg_lua_helpers.c` — new and updated helpers

#### 4a. `pg_result.h` — result types and shared API (new file)

`pg_result_t`, the sizing macros, the thread-local declaration, and all helper
prototypes live in a new internal header so that `postgres.c`, `pg_lua_helpers.c`,
and `pg_quickjs_helpers.c` all compile against the same definitions without
circular includes.  `pg_lua_helpers.h` (which only the Lua glue needs) is a
separate file and does not define `pg_result_t`.

```c
/* extensions/postgres/pg_result.h */
#ifndef PG_RESULT_H
#define PG_RESULT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "pg_message.h"   /* pg_col_desc_t, PG_RESULT_MAX_COLS */

#define PG_RESULT_MAX_ROWS   256
#define PG_RESULT_HEAP_SIZE  (256 * 1024)   /* 256 KB per thread */

typedef struct {
    pg_col_desc_t  cols[PG_RESULT_MAX_COLS];
    int16_t        ncols;
    struct {
        const char *value;  /* NULL for SQL NULL; points into heap */
        size_t      len;
    } fields[PG_RESULT_MAX_ROWS][PG_RESULT_MAX_COLS];
    int32_t        nrows;
    char           cmd_tag[64];
    uint8_t        pg_status;    /* 'I', 'T', or 'E' from final ReadyForQuery */
    bool           valid;
    char           heap[PG_RESULT_HEAP_SIZE];
    size_t         heap_used;
} pg_result_t;

/* Defined in pg_lua_helpers.c; one instance per OS thread. */
extern __thread pg_result_t tls_result;

void pg_result_reset(void);
void pg_result_set_columns(const pg_col_desc_t *cols, int16_t ncols);
void pg_result_append_row(const pg_col_desc_t *cols, int16_t ncols,
                          const pg_data_row_t *row, int consumed);
void pg_result_set_cmd_tag(const char *tag);

#endif /* PG_RESULT_H */
```

`__thread` is a GCC/Clang extension supported on both Linux (glibc) and macOS
(libSystem); `-D_REENTRANT` is already in `CFLAGS`.

`pg_lua_helpers.c` provides the `__thread pg_result_t tls_result;` definition
and implements all five functions.  `postgres.c` and `pg_quickjs_helpers.c`
include `pg_result.h` only.

**`pg_result_reset()`** — zeros `tls_result.nrows`, `ncols`, `heap_used`,
`valid`, `cmd_tag[0]` (set to `'\0'`), and `pg_status` (set to `0`); preserves
the heap buffer itself.  Clearing `cmd_tag` and `pg_status` prevents stale
values from prior responses appearing in `pg.result()` after unusual sequences
(e.g. an error response followed by `ReadyForQuery` with no `CommandComplete`).

**`pg_result_set_columns(cols, ncols)`** — copies `ncols` descriptors from
`cols` into `tls_result.cols`; sets `tls_result.ncols`.  Called from
`readable()` on `PG_MSG_ROW_DESCRIPTION`; must be called even for queries that
return zero rows so that `pg.result().cols` is populated.

**`pg_result_append_row(cols, ncols, row, consumed)`** — takes a `const
pg_data_row_t *` (the named type from `pg_message.h`); copies field values into
`tls_result.heap`; stores `value`/`len` pointers in
`tls_result.fields[nrows][]`.  If `nrows >= PG_RESULT_MAX_ROWS` or `heap_used +
total_len > PG_RESULT_HEAP_SIZE`, silently truncates (row is not stored).

**`pg_result_set_cmd_tag(tag)`** — copies `tag` into `tls_result.cmd_tag`.

#### 4b. `pg.result()` — Lua result access

Called from a `response()` hook; reads `tls_result` and pushes a table.

```c
static int lua_pg_result(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (!tls_result.valid) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);  /* result table */

    /* result.ncols */
    lua_pushinteger(L, tls_result.ncols);
    lua_setfield(L, -2, "ncols");

    /* result.nrows */
    lua_pushinteger(L, tls_result.nrows);
    lua_setfield(L, -2, "nrows");

    /* result.cmd_tag */
    lua_pushstring(L, tls_result.cmd_tag);
    lua_setfield(L, -2, "cmd_tag");

    /* result.status — 'I', 'T', or 'E' as a one-character string */
    char status_str[2] = { (char)tls_result.pg_status, '\0' };
    lua_pushstring(L, status_str);
    lua_setfield(L, -2, "status");

    /* result.cols — array of column name strings */
    lua_newtable(L);
    for (int c = 0; c < tls_result.ncols; c++) {
        lua_pushstring(L, tls_result.cols[c].name);
        lua_rawseti(L, -2, c + 1);
    }
    lua_setfield(L, -2, "cols");

    /* result.rows — array of arrays of strings (nil for SQL NULL) */
    lua_newtable(L);
    for (int r = 0; r < tls_result.nrows; r++) {
        lua_newtable(L);
        for (int c = 0; c < tls_result.ncols; c++) {
            const char *v = tls_result.fields[r][c].value;
            if (v == NULL) {
                lua_pushnil(L);
            } else {
                lua_pushlstring(L, v, tls_result.fields[r][c].len);
            }
            lua_rawseti(L, -2, c + 1);
        }
        lua_rawseti(L, -2, r + 1);
    }
    lua_setfield(L, -2, "rows");

    return 1;
}
```

**Design note:** `pg.result()` is valid only inside `response()`; calling it from
`request()` returns the previous query's result (likely stale) or nil.

#### 4c. Transaction helpers

These are thin wrappers over `pg_encode_query`:

```c
static int lua_pg_begin(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    char buf[16]; int n = pg_encode_query(buf, sizeof(buf), "BEGIN");
    lua_pushlstring(L, buf, (size_t)n); return 1;
}
static int lua_pg_commit(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    char buf[16]; int n = pg_encode_query(buf, sizeof(buf), "COMMIT");
    lua_pushlstring(L, buf, (size_t)n); return 1;
}
static int lua_pg_rollback(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    char buf[16]; int n = pg_encode_query(buf, sizeof(buf), "ROLLBACK");
    lua_pushlstring(L, buf, (size_t)n); return 1;
}
```

Updated helper table:

```c
const script_helper postgres_lua_helpers[] = {
    { "query",    lua_pg_query    },
    { "prepare",  lua_pg_prepare  },
    { "execute",  lua_pg_execute  },
    { "result",   lua_pg_result   },
    { "begin",    lua_pg_begin    },
    { "commit",   lua_pg_commit   },
    { "rollback", lua_pg_rollback },
};
```

---

### 5. `extensions/postgres/pg_quickjs_helpers.c` + `pg_quickjs_helpers.h`

Mirror all seven Lua helpers under the `"postgres@quickjs"` namespace, following
the `redis_quickjs_helpers.c` pattern exactly.

The `qjs_helper_ctx` struct layout is defined once in
`extensions/redis/redis_quickjs_helpers.h` with a LAYOUT CONTRACT comment.  Copy
the same typedef into `pg_quickjs_helpers.h` (same struct, same fields, same
comment).

**Helper mapping:**

| Lua helper        | QuickJS helper        | Notes |
|---|---|---|
| `lua_pg_query`    | `qjs_pg_query`        | `JS_ToCStringLen` + `JS_NewStringLen` |
| `lua_pg_prepare`  | `qjs_pg_prepare`      | returns `JS_NewObject` with `sql` property |
| `lua_pg_execute`  | `qjs_pg_execute`      | same logic; nil params → `JS_IsNull/Undefined` |
| `lua_pg_result`   | `qjs_pg_result`       | build JS Object with arrays |
| `lua_pg_begin`    | `qjs_pg_begin`        | trivial |
| `lua_pg_commit`   | `qjs_pg_commit`       | trivial |
| `lua_pg_rollback` | `qjs_pg_rollback`     | trivial |

The thread-local `tls_result` is shared via `pg_result.h` (see deliverable 4a);
`pg_quickjs_helpers.c` includes `pg_result.h` to reach `tls_result` and the
`pg_result_*` helpers without duplicating storage.

`pg_quickjs_helpers.c` is gated on `#ifdef WRKX_HAVE_QUICKJS` (same pattern as
redis).

---

### 6. `extensions/postgres/init.c` — schema and helper registration

```c
void wrkx_extension_init_postgres(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;
    api->register_protocol(postgres_protocol());

    api->register_helpers("postgres@lua",
                          postgres_lua_helpers, postgres_lua_helpers_count);

#ifdef WRKX_HAVE_QUICKJS
    api->register_helpers("postgres@quickjs",
                          postgres_quickjs_helpers, postgres_quickjs_helpers_count);
#endif

    /* Replace the P6-1 plain registrations: pass the TLS variant name as
     * schema_tls so the host sets ssl_ctx != NULL when the user supplies
     * postgres+tls:// or postgresql+ssl://.  detect_protocol() checks the
     * plain schema first; passing it as both args would match as plain and
     * leave ssl_ctx NULL.  connect() checks ssl_ctx to decide whether to
     * send the SSLRequest prelude. */
    api->register_schema("postgres",   "postgres+tls",  "5432",
                         postgres_configure_cb);
    api->register_schema("postgresql", "postgresql+ssl", "5432",
                         postgres_configure_cb);
}
```

`postgres_configure_cb` is unchanged; it stores `ssl_ctx` from `wrkx_connect_info`
into `g_cfg.ssl_ctx` regardless of schema variant.  The TLS prelude in `connect()`
checks `g_cfg.ssl_ctx != NULL`.

---

### 7. `extensions/postgres/Makefile.ext`

```makefile
EXT_SRCS     += extensions/postgres/postgres.c \
                extensions/postgres/pg_message.c \
                extensions/postgres/pg_scram.c \
                extensions/postgres/pg_lua_helpers.c \
                extensions/postgres/pg_quickjs_helpers.c \
                extensions/postgres/init.c
EXT_INIT_FNS += wrkx_extension_init_postgres
EXT_CFLAGS   += -Iextensions/postgres
```

---

### 8. `tests/unit/test_pg_codec.c` — SCRAM codec tests

Extend the existing test file (P6-1 + P6-2 tests already present) with:

```
test_parse_auth_sasl_continue
    R message with authtype=11 and a server-first-message string →
    PG_MSG_AUTH_SASL_CONTINUE, .sasl_continue.data correct, .len correct

test_parse_auth_sasl_final
    R message with authtype=12 and "v=<base64>" →
    PG_MSG_AUTH_SASL_FINAL, .sasl_final.data correct

test_encode_sasl_initial_response
    pg_encode_sasl_initial_response(buf, cap, "SCRAM-SHA-256", "n,,n=alice,r=xyz", 15) →
    'p' tag, length field correct, mechanism + \0 + int32(client_first_len) + client_first

test_encode_sasl_response
    pg_encode_sasl_response(buf, cap, "c=biws,r=combined,p=proof", 25) →
    'p' tag, length correct, payload = client_final verbatim (no null terminator)

test_parse_ready_for_query_status_idle
    "\x5a\x00\x00\x00\x05\x49" → PG_MSG_READY_FOR_QUERY, rfq.status = 'I'

test_parse_ready_for_query_status_error
    "\x5a\x00\x00\x00\x05\x45" → PG_MSG_READY_FOR_QUERY, rfq.status = 'E'

test_parse_row_description_with_columns
    two-column description with names "id" and "val", type OIDs 23 and 25 →
    PG_MSG_ROW_DESCRIPTION, ncols=2, cols[0].name="id", cols[1].name="val",
    cols[0].type_oid=23, cols[1].type_oid=25

test_parse_data_row_with_values
    two-field row: int4 "42" (3 bytes) + text "hello" (5 bytes) →
    PG_MSG_DATA_ROW, nfields=2, fields[0].len=3 fields[0].data="42",
    fields[1].len=5 fields[1].data="hello"

test_parse_data_row_with_null
    two-field row: NULL (int32(-1)) + "foo" →
    nfields=2, fields[0].len=-1 fields[0].data=NULL, fields[1].data="foo"
```

---

### 9. `tests/unit/test_pg_scram.c` — SCRAM unit tests

New test binary; links only `pg_scram.c` + OpenSSL (no LuaJIT, no protocol code).

```
test_client_first_generates_nonce
    pg_scram_client_first("alice", nonce, buf, cap) → buf starts with "n=alice,r="
    (bare message only; no "n,," GS2 header),
    nonce is 18+ printable ASCII chars, no commas

test_client_final_known_vector
    Known RFC 5802 test vector (username="user" password="pencil"
    client-nonce="fyko+d2lbbFgONRv9qkxdawL"
    server-first="r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096"):
    client-final contains correct "p=" base64 value (compare against RFC 5802 example)

test_server_verify_correct_signature
    Using the RFC 5802 vector: call pg_scram_client_final with known inputs to
    obtain expected_server_sig; then pass the RFC vector server-final
    "v=rmF9pqV8S7suAoZWja4dJRkFsKQ=" → pg_scram_verify_server returns 1.
    (pg_scram_verify_server is tested via expected_server_sig from client_final,
    not via a raw ServerKey — tests the full API chain.)

test_server_verify_wrong_signature
    Corrupt one byte of expected_server_sig before calling pg_scram_verify_server →
    returns 0

test_client_final_rejects_wrong_nonce
    server-first with r= not starting with client nonce → returns -1

test_client_final_rejects_missing_s_field
    server-first without "s=" → returns -1

test_client_final_rejects_zero_iterations
    server-first with "i=0" → returns -1 (protect against DoS)

test_client_first_username_escaping
    pg_scram_client_first("a,b=c", nonce, buf, cap) →
    buf contains "n=a=2Cb=3Dc,r=..." (comma → "=2C", equals → "=3D")
```

**Makefile additions:**

```makefile
TEST_PG_SCRAM_SRC := tests/unit/test_pg_scram.c
TEST_PG_SCRAM_BIN := obj/test_pg_scram
PG_SCRAM_DEPS     := extensions/postgres/pg_scram.c

$(TEST_PG_SCRAM_BIN): $(TEST_PG_SCRAM_SRC) $(UNITY_SRC) $(PG_SCRAM_DEPS) | $(ODIR)
	$(CC) $(CFLAGS) $(UNITY_INC) -Iextensions/postgres -Isrc \
	      -o $@ $(TEST_PG_SCRAM_SRC) $(UNITY_SRC) $(PG_SCRAM_DEPS) $(LIBS)
```

---

### 10. `tests/unit/test_pg_lua.c` — additions for new helpers

Extend the P6-2 Lua test file with:

```
test_pg_result_nil_before_response
    pg.result() called with no prior response cycle → returns nil, no crash

test_pg_result_table_structure
    Simulate a result (manually populate tls_result) → pg.result() returns
    table with correct ncols, nrows, cols array, rows array, cmd_tag, status

test_pg_result_null_field
    Row with a SQL NULL field → rows[1][1] == nil in Lua

test_pg_begin_returns_begin_wire
    pg.begin() → wire bytes with 'Q' tag and "BEGIN\0"

test_pg_commit_returns_commit_wire
    pg.commit() → wire bytes with 'Q' tag and "COMMIT\0"

test_pg_rollback_returns_rollback_wire
    pg.rollback() → wire bytes with 'Q' tag and "ROLLBACK\0"

test_pg_begin_commit_concat_pending_count
    pg.begin() .. pg.execute("SELECT 1") .. pg.commit() →
    count_rfq_expected returns 3 (one Q + one Sync + one Q)
    [test count_rfq_expected directly; it is defined static in postgres.c;
     expose via a test-only wrapper or make it non-static in postgres.h]
```

---

### 11. `scripts/postgres_transaction.lua` — example transaction workload

```lua
-- scripts/postgres_transaction.lua
--
-- PostgreSQL transaction workload for wrkx (ADR 0005, P6-3).
-- Issues BEGIN + INSERT + COMMIT as a single pipelined request.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/postgres_transaction.lua \
--          postgres+tls://wrkx:secret@localhost/wrkx

local counter = 0

function request()
    counter = counter + 1
    local id  = counter % 10000
    local val = tostring(id * 7)
    return pg.begin()
        .. pg.execute("INSERT INTO bench(id, val) VALUES($1, $2)",
                       tostring(id), val)
        .. pg.commit()
end

function response(status, _, _)
    if status ~= 0 then return end
    local r = pg.result()
    if r and r.status == 'E' then
        -- Transaction rolled back — count but don't crash
    end
end
```

---

### 12. `scripts/postgres_tls_select.lua` — TLS smoke workload

```lua
-- scripts/postgres_tls_select.lua
--
-- Smoke test for postgres+tls:// (ADR 0005, P6-3).
-- Use with:
--   ./wrkx -t2 -c10 -d5s -R50 -s scripts/postgres_tls_select.lua \
--          postgres+tls://wrkx:secret@localhost/wrkx

function request()
    return pg.query("SELECT 1")
end
```

---

### 13. `tests/e2e/postgres_session.sh` — E2E test

Requires `POSTGRES_URL` (skip cleanly if absent); tests TLS separately via
`POSTGRES_TLS_URL` (skip TLS tests if absent, not a failure).

```
PASS  SCRAM auth: POSTGRES_SCRAM_URL=postgres://scram_user:pw@host/db connects and runs SELECT 1
      (skip cleanly if POSTGRES_SCRAM_URL unset)

PASS  TLS connection: postgres+tls://user:pw@host/db connects and runs SELECT 1
      (skip cleanly if POSTGRES_TLS_URL unset)

PASS  pg.result() columns: script using response() hook to assert result.ncols > 0
      exits 0, reports > 0 requests

PASS  pg.result() rows: SELECT 1 AS x → result.rows[1][1] == "1"

PASS  transaction smoke: pg.begin()..pg.execute()..pg.commit() workload
      -t1 -c1 -d5s -R20, > 0 requests, 0 errors (requires INSERT-capable table)

PASS  pg.rollback() on error: script that issues a deliberately bad SQL inside a
      transaction, then calls pg.rollback(); connection survives, next request succeeds

PASS  multi-query pipelining count: pg.begin()..pg.execute()..pg.commit() produces
      queries_pending=3; confirm by measuring ReadyForQuery messages in a pcap OR
      simply assert 0 errors and > 0 requests (integration-level smoke)

PASS  pg.query(), pg.execute() regression (postgres_basic.sh + postgres_prepared.sh
      still pass unchanged with postgres+plain URL)

PASS  frozen core unchanged since HEAD
```

---

## Guards

1. `make test` — `test_pg_codec`, `test_pg_scram`, `test_pg_lua` all pass; all
   P6-1 and P6-2 tests still pass
2. `make EXTENSIONS="redis memcached postgres" test-asan` — clean under ASAN;
   no leaks in SCRAM code, result heap, or TLS path.  `test_pg_lua` excluded from
   ASAN (LuaJIT not ASAN-clean, per existing Makefile policy).
3. `POSTGRES_URL=... make EXTENSIONS="redis memcached postgres" test` — all three
   E2E test scripts (`postgres_basic.sh`, `postgres_prepared.sh`,
   `postgres_session.sh`) pass.  TLS and SCRAM subtests skip cleanly if their
   respective env vars are unset.
4. `scripts/adr-compliance.sh` passes — no new `src/` includes from
   `extensions/postgres/`.
5. Frozen-file diff is empty: `src/orchestrator.*`, `src/ae.*`, `src/rate.*`,
   `src/net.*`, `src/transport.*`, `include/wrkx_extension.h`,
   `include/wrkx_transport.h`, `extensions/redis/`.
6. `pg.query()` and `pg.execute()` regression: P6-1 and P6-2 E2E suites pass
   unchanged with a plain `postgres://` URL.

## Core engine touch

Zero.  All changes are within `extensions/postgres/` and `tests/unit/`,
`tests/e2e/`, `scripts/`.

`wrkx_extension.h` and `include/wrkx_transport.h` must not change.

The `transport` struct fields (`ssl`, `ssl_ctx`, `handshaking`) are manipulated
directly in `postgres.c` because the extension owns its embedded `transport xport`
and the struct is defined in the public header `include/wrkx_transport.h` — this
is not a frozen-file violation.

## Out of scope for P6-3

- **Named prepared statements with per-connection lifecycle** — tracking "has
  connection N parsed statement X?" requires either per-connection Lua state
  (not available in the current scripting model) or vtable-level interception of
  the wire bytes in `write()`.  Defer to a follow-on task after the scripting
  model is richer.
- **`sslmode=prefer`** — try TLS, fall back to plain if server sends 'N'.  Adds
  retry logic to `connect()` and a URL query-parameter parser.  Defer.
- **SSL certificate verification config** — `ssl_ctx` is built by the host before
  calling `postgres_configure_cb`; certificate policy (verify-full, verify-ca,
  skip-verify) requires either a new `wrkx_connect_info` field or a URL query
  parameter (`sslmode=verify-full`).  Defer.
- **Binary protocol format** — PostgreSQL supports binary result encoding via the
  format codes in `Bind`; text format is sufficient for P6-3.
- **SCRAM channel binding (`SCRAM-SHA-256-PLUS`)** — requires TLS channel
  binding data (`tls-exporter`); the current transport layer does not expose it.
  If the server only advertises `-PLUS`, log an error and fail.
- **MySQL prepared statements** — P6-4.
- **QuickJS pg `result()` object** — fully parallel to the Lua implementation;
  the JS object uses `JS_NewObject`, `JS_NewArray`, `JS_NewString`.
  Included in this task (deliverable 5 above) but separated here as a note:
  if QuickJS is not linked, `pg_quickjs_helpers.c` compiles to an empty
  translation unit (gated on `#ifdef WRKX_HAVE_QUICKJS`).
