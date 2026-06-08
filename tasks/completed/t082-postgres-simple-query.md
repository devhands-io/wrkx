title: postgres extension — P6-1 simple query (startup, auth, Q/response, Lua helper)
status: completed
adr: 0005
adr-step: P6-1

## Why / Goal

Phase 6 (ADR 0005) proves that complex, multi-step, stateful protocol lifecycles can
be expressed through the protocol vtable without leaking state into the orchestrator
or request layer.  P6-1 is the foundation: a working PostgreSQL extension that can
connect, authenticate, issue a simple `Q` query, parse the response, and expose
`pg.query(sql)` to Lua scripts.

Gate E is not confirmed here (that requires prepared statements at P6-2), but P6-1
must leave the frozen core untouched, establishing the baseline.

**Before writing any code for this task**, tag the current HEAD as `p6-baseline`:

```bash
git tag p6-baseline
```

This tag is required by the Gate E check in t083 (`postgres_prepared.sh`).  It must
point to a commit with no `extensions/postgres/` code.

---

## Deliverables

### 1. `extensions/postgres/pg_message.c` + `pg_message.h` — wire codec

PostgreSQL uses a tag-length-value framing for all post-startup messages.
The startup message is a special case (no tag byte).

**Message types to encode:**

| Function | Wire format |
|---|---|
| `pg_encode_startup(buf, len, user, db)` | `int32 len, int32 0x30000, "user\0<user>\0database\0<db>\0\0"` |
| `pg_encode_query(buf, len, sql)` | `'Q', int32 len+5, sql, '\0'` |
| `pg_encode_password(buf, len, password)` | `'p', int32 len+5, password, '\0'` |
| `pg_encode_md5_password(buf, len, password, user, salt)` | `'p', int32 len+5, "md5" + hex(MD5(MD5(password+user)+salt)), '\0'` |

MD5 is available via OpenSSL (`EVP_MD_CTX`, `EVP_md5()`).  No new dependency.

**Message types to parse (incremental — returns bytes consumed or 0 if incomplete):**

```c
typedef enum {
    PG_MSG_UNKNOWN = 0,
    PG_MSG_AUTH_OK,           /* R, authtype=0                               */
    PG_MSG_AUTH_CLEARTEXT,    /* R, authtype=3                               */
    PG_MSG_AUTH_MD5,          /* R, authtype=5, includes 4-byte salt         */
    PG_MSG_AUTH_SASL,         /* R, authtype=10 — parsed but rejected P6-1  */
    PG_MSG_ROW_DESCRIPTION,   /* T — record column count only                */
    PG_MSG_DATA_ROW,          /* D — record row count, skip field data       */
    PG_MSG_COMMAND_COMPLETE,  /* C — null-terminated tag string              */
    PG_MSG_READY_FOR_QUERY,   /* Z — signals end of response cycle           */
    PG_MSG_ERROR_RESPONSE,    /* E — collect SEVERITY + MESSAGE fields       */
    PG_MSG_NOTICE_RESPONSE,   /* N — ignore silently                         */
    PG_MSG_PARAMETER_STATUS,  /* S — ignore silently                         */
    PG_MSG_BACKEND_KEY_DATA,  /* K — store cancel key (deferred use)         */
} pg_msg_type;

typedef struct pg_parsed_msg {
    pg_msg_type type;
    union {
        struct { uint8_t salt[4]; } md5;
        struct { char    tag[64]; } cmd_complete;     /* "SELECT 1", "SET", … */
        struct { char    severity[16]; char message[256]; } error;
        struct { int16_t ncols;  } row_description;
        struct { int16_t nfields; } data_row;
        struct { char    sasl_mechanisms[128]; } sasl; /* first only */
        struct { uint32_t pid; uint32_t key; } backend_key;
    };
} pg_parsed_msg;

/* Returns bytes consumed (>0), 0 if more data needed, -1 on parse error. */
int pg_parse_message(const char *buf, size_t len, pg_parsed_msg *out);
```

P6-1 does **not** need to decode field values from `DataRow` — it only needs to
count rows and accumulate `bytes`.  Full result decoding belongs in P6-3 scripting
helpers once a result object model exists.

**SASL / SCRAM-SHA-256:** parse the `AuthenticationSASL` message to extract the
mechanism list, then return an error:
```
pg: SCRAM-SHA-256 auth not supported in P6-1; use md5 or trust in pg_hba.conf
```
This gives a clear failure message instead of a hang.  Implement SCRAM in P6-3.

---

### 2. `extensions/postgres/postgres.c` + `postgres.h` — protocol vtable

```c
static struct {
    struct addrinfo *addr;
    SSL_CTX         *ssl_ctx;
    const char      *host;
    const char      *user;
    const char      *password;
    const char      *dbname;
} g_cfg;
```

URL parsing conventions:
- `postgres://[user[:password]@]host[:port]/[dbname]`
- `postgresql://` is a synonym
- Default port: `5432`
- `user` defaults to `"wrkx"`; `dbname` defaults to `user` (PostgreSQL convention)
- TLS schemas (`postgres+tls://`, `postgresql+ssl://`) are **out of scope for P6-1**
  (see below); only plain TCP is supported here

**`postgres_configure_cb(wrkx_connect_info *)`** — populate `g_cfg` from info.

`info->password` is the raw `UF_USERINFO` field from the URL parser — it contains
the entire userinfo string, e.g. `"alice:secret"` for
`postgres://alice:secret@host/db`, not the password alone.  The callback must split
on the first `:`:

```c
const char *userinfo = info->password;   /* may be NULL */
if (userinfo) {
    const char *colon = strchr(userinfo, ':');
    if (colon) {
        /* strndup is POSIX, not C99; use malloc+memcpy to stay within -std=c99 */
        size_t ulen   = (size_t)(colon - userinfo);
        char  *ubuf   = malloc(ulen + 1);
        if (!ubuf) return;
        memcpy(ubuf, userinfo, ulen);
        ubuf[ulen]     = '\0';
        g_cfg.user     = ubuf;
        g_cfg.password = strdup(colon + 1);
    } else {
        g_cfg.user     = strdup(userinfo);  /* user only, no password (trust) */
        g_cfg.password = NULL;
    }
} else {
    g_cfg.user     = strdup("wrkx");
    g_cfg.password = NULL;
}
```

`dbname` is parsed from `info->path`: strip the leading `/`; if empty, default to
`g_cfg.user`.

**Per-connection state:**

```c
#define PG_RECVBUF 32768

typedef enum {
    PG_PHASE_STARTUP,     /* sent StartupMessage, awaiting auth       */
    PG_PHASE_AUTH,        /* sent password response, awaiting AuthOK  */
    PG_PHASE_READY,       /* received ReadyForQuery, idle             */
    PG_PHASE_QUERY,       /* sent Q message, awaiting response        */
    PG_PHASE_ERROR,       /* unrecoverable error                      */
} pg_phase;

typedef struct pg_state {
    transport  xport;
    char       rbuf[PG_RECVBUF];
    size_t     rbuf_len;
    pg_phase   phase;
    bool       done;          /* response cycle complete               */
    bool       error;
    size_t     bytes;         /* wire bytes of last completed response */
    int32_t    row_count;     /* rows received in current query        */
} pg_state;
```

**`connect(connection *c)`** — synchronous startup + auth handshake, same pattern
as `redis_connect`:

```
1. calloc pg_state; transport_init; transport_connect
2. send pg_encode_startup()
3. loop: sync_recv_pg_message()
   - AUTH_OK        → authenticated = true; loop   (do NOT break here — server
                       still sends ParameterStatus* + BackendKeyData + ReadyForQuery;
                       breaking on AUTH_OK leaves those messages unread and the first
                       readable() call will consume the startup ReadyForQuery instead
                       of a real query response)
   - AUTH_CLEARTEXT → send pg_encode_password(); loop
   - AUTH_MD5       → send pg_encode_md5_password(); loop
   - AUTH_SASL      → log error; goto fail
   - PARAMETER_STATUS, BACKEND_KEY_DATA, NOTICE → ignore; loop
   - READY_FOR_QUERY → phase = PG_PHASE_READY; break   (this is the correct exit)
   - ERROR_RESPONSE  → log error.message; goto fail
   - timeout (PG_AUTH_TIMEOUT_MS = 10000) → goto fail
4. c->proto_state = s; return 0
```

The only correct exit from the startup loop is `READY_FOR_QUERY`.  `AUTH_OK` merely
records that authentication was accepted; the server always follows it with
`ParameterStatus` messages, optionally `BackendKeyData`, and finally
`ReadyForQuery`.

`sync_recv_pg_message` uses `poll(POLLIN)` + `recv` into a scratch buffer, calling
`pg_parse_message` until it returns > 0.

**`write(connection *c, buf, len)`** — forward bytes via `transport_write`; set
`s->phase = PG_PHASE_QUERY`; reset `s->done`, `s->error`, `s->bytes`,
`s->row_count`.

**`readable(connection *c)`** — non-blocking, called repeatedly from the event loop:

```
1. transport_handshake (in case TLS is still completing)
2. transport_read into rbuf (RETRY → PENDING; EOF/error → PROTO_ERROR)
3. loop: pg_parse_message(rbuf, rbuf_len, &msg)
   - READY_FOR_QUERY  → s->done=true; consume; c->bytes = s->bytes; break
   - ROW_DESCRIPTION  → consume
   - DATA_ROW         → s->bytes += consumed; s->row_count++; consume
   - COMMAND_COMPLETE → s->bytes += consumed; consume
   - ERROR_RESPONSE   → s->error=true; consume (keep reading to READY_FOR_QUERY)
   - PARAMETER_STATUS, NOTICE → consume (can arrive mid-query)
   - 0 (incomplete)   → break → return PENDING
   - -1 (parse error) → return PROTO_ERROR
4. if s->done: return s->error ? PROTO_DONE_STATUS_ERR : PROTO_DONE
5. return PROTO_PENDING
```

**`close(connection *c)`** — `transport_close`; free `pg_state`.

---

### 3. `extensions/postgres/pg_lua_helpers.c` + `pg_lua_helpers.h`

**`pg.query(sql)`** — Lua helper that encodes a simple `Q` message and returns it
as a Lua string (wire bytes), exactly like `redis.command()` returns RESP bytes.

```c
static int lua_pg_query(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "pg.query: expected one string argument");

    size_t sql_len;
    const char *sql = lua_tolstring(L, 1, &sql_len);

    char buf[65536];
    int n = pg_encode_query(buf, sizeof(buf), sql);
    if (n <= 0)
        return luaL_error(L, "pg.query: SQL too large");

    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}
```

Namespace: `"postgres@lua"` (following the `redis@lua` convention).

No other helpers in P6-1.  `pg.prepare()` is P6-2; result-set access helpers are
P6-3.

---

### 4. `extensions/postgres/init.c` — extension entry point

```c
void wrkx_extension_init_postgres(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;
    api->register_protocol(postgres_protocol());
    api->register_helpers("postgres@lua",
                          postgres_lua_helpers, postgres_lua_helpers_count);
    /* TLS schemas deferred to P6-3 (requires PostgreSQL SSLRequest prelude). */
    api->register_schema("postgres",   NULL, "5432", postgres_configure_cb);
    api->register_schema("postgresql", NULL, "5432", postgres_configure_cb);
}
```

---

### 5. `extensions/postgres/Makefile.ext` + `Makefile` integration

```makefile
# extensions/postgres/Makefile.ext
EXT_SRCS     += extensions/postgres/postgres.c \
                extensions/postgres/pg_message.c \
                extensions/postgres/pg_lua_helpers.c \
                extensions/postgres/init.c
EXT_INIT_FNS += wrkx_extension_init_postgres
EXT_CFLAGS   += -Iextensions/postgres
```

`Makefile` changes (mirroring the `TEST_MC_CODEC_*` pattern — codec test links only
the codec source, not the full protocol or transport):

```makefile
TEST_PG_CODEC_SRC := tests/unit/test_pg_codec.c
TEST_PG_CODEC_BIN := obj/test_pg_codec
PG_CODEC_DEPS     := extensions/postgres/pg_message.c

$(TEST_PG_CODEC_BIN): $(TEST_PG_CODEC_SRC) $(UNITY_SRC) $(PG_CODEC_DEPS) | $(ODIR)
	$(CC) $(CFLAGS) $(UNITY_INC) -Iextensions/postgres -Isrc \
	      -o $@ $(TEST_PG_CODEC_SRC) $(UNITY_SRC) $(PG_CODEC_DEPS) $(LIBS)

# Add TEST_PG_CODEC_BIN to test-unit prereqs and run it in the test-unit recipe
# alongside TEST_MC_CODEC_BIN etc.
```

The E2E test requires a binary built with the postgres extension:
```
POSTGRES_URL=postgres://user:pw@host/db make EXTENSIONS="redis memcached postgres" test
```

The E2E script checks `POSTGRES_URL` and skips cleanly if absent; the binary check
uses the registered schema list (attempting a `postgres://` URL fails fast with
"unknown schema" if the extension was not compiled in).

---

### 6. `tests/unit/test_pg_codec.c` — message codec unit tests

Tests for `pg_message.c` in isolation (no TCP, no engine):

```
test_encode_startup_format
    encodes correct length, protocol version 0x30000, user/database key-value pairs

test_encode_query_format
    'Q' tag, correct length, null-terminated SQL

test_encode_password_cleartext
    'p' tag, correct length, null-terminated password string

test_encode_md5_password
    known input → known MD5 hex digest; regression: user="wrkx" password="secret"
    salt="\x01\x02\x03\x04" → expected digest verified by hand / psql source

test_parse_auth_ok
    "\x52\x00\x00\x00\x08\x00\x00\x00\x00" → PG_MSG_AUTH_OK, 9 bytes consumed

test_parse_auth_cleartext
    → PG_MSG_AUTH_CLEARTEXT

test_parse_auth_md5
    → PG_MSG_AUTH_MD5, salt fields correct

test_parse_auth_sasl
    → PG_MSG_AUTH_SASL, first mechanism in .sasl.sasl_mechanisms

test_parse_ready_for_query
    "\x5a\x00\x00\x00\x05\x49" (status='I') → PG_MSG_READY_FOR_QUERY

test_parse_command_complete
    "\x43\x00\x00\x00\x0bSELECT 1\x00" → PG_MSG_COMMAND_COMPLETE, tag="SELECT 1"

test_parse_error_response
    error with S (SEVERITY) and M (MESSAGE) fields → correct extraction

test_parse_data_row
    one-field row → PG_MSG_DATA_ROW, nfields=1

test_parse_row_description
    three-column description → PG_MSG_ROW_DESCRIPTION, ncols=3

test_parse_incomplete_returns_zero
    truncated input at any byte → returns 0 (no crash)

test_parse_unknown_tag_consumed
    tag 'X' with valid length → PG_MSG_UNKNOWN, length bytes consumed
```

---

### 7. `scripts/postgres_select.lua` — example workload

```lua
-- scripts/postgres_select.lua
--
-- Basic PostgreSQL workload for wrkx (ADR 0005, P6-1).
-- Issues a simple SELECT against a counter key.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/postgres_select.lua \
--          postgres://wrkx:secret@localhost/wrkx

local counter = 0

function request()
    counter = counter + 1
    local key = counter % 100
    return pg.query(string.format("SELECT %d", key))
end
```

---

### 8. `tests/e2e/postgres_basic.sh` — E2E test

Requires `POSTGRES_URL` env var (skip with clear message if unset).

```
PASS  postgres extension builds (make EXTENSIONS="redis memcached postgres")
PASS  connection smoke test: -t1 -c1 -d2s -R10 SELECT 1 exits 0, reports > 0 requests
PASS  throughput smoke test: -t1 -c4 -d5s -R100 reports > 0 requests, 0 errors
PASS  error query (syntax error SQL) reports non-zero error count, does not crash
PASS  wrong password returns connection error within PG_AUTH_TIMEOUT_MS, not hang
PASS  no core engine changes since HEAD (frozen-file diff clean)
```

The "no core engine changes" check diffs the same frozen paths as `gate_d.sh`
against `HEAD` (not a baseline tag, since P6 starts a new baseline).

---

## Guards

1. `make test` — `test_pg_codec` passes (links only `pg_message.c`; no postgres
   extension in binary required); all existing tests still pass
2. `make EXTENSIONS="redis memcached postgres" test-asan` — clean under ASAN; no
   memory leaks in codec or connection lifecycle
3. `POSTGRES_URL=postgres://user:pw@host/db make EXTENSIONS="redis memcached postgres" test`
   — `tests/e2e/postgres_basic.sh` passes; test skips cleanly if `POSTGRES_URL` is
   absent; fails with a clear error if the postgres extension was not compiled in
4. Frozen-file diff is empty: `src/orchestrator.*`, `src/ae.*`, `src/rate.*`,
   `src/net.*`, `src/transport.*`, `include/wrkx_extension.h`,
   `include/wrkx_transport.h`, `extensions/redis/`
5. `scripts/adr-compliance.sh` passes (no new includes of private core headers from
   `extensions/postgres/`)

## Core engine touch

Zero.  New code is entirely within `extensions/postgres/` and `tests/unit/`,
`tests/e2e/`, `scripts/`.

`wrkx_extension.h` must **not** change.  If the PostgreSQL extension reveals a gap
in the extension API, record it and address it in a separate task before P6-2.

## Out of scope for P6-1

- TLS (`postgres+tls://`, `postgresql+ssl://`) — PostgreSQL TLS requires an
  `SSLRequest` packet + single-byte `S`/`N` response before the TLS handshake; the
  current `transport_connect`/`transport_handshake` path assumes direct TLS and will
  fail against standard servers.  Implement the SSLRequest prelude in P6-3.
- SCRAM-SHA-256 auth (P6-3)
- Prepared statements / extended query protocol (P6-2)
- Transaction-oriented workloads (P6-3)
- Result-set field decoding / Lua result objects (P6-3)
- QuickJS helpers (small follow-on after P6-1)
- Binary protocol format (P6-3)
- SSL certificate verification config (P6-3)
