title: mysql extension — P6-4 simple query (handshake, COM_QUERY, resultset, Lua helper)
status: completed
adr: 0005
adr-step: P6-4
depends: t084

## Why / Goal

P6-4 adds the MySQL extension to wrkx.  It proves that a second, meaningfully
different database protocol (MySQL Client/Server protocol) can be expressed
through the same protocol vtable as PostgreSQL without touching the orchestrator,
scheduler, or extension API.

The MySQL wire protocol differs from PostgreSQL in every dimension that matters
architecturally: packet framing (3-byte LE length + sequence number vs.
tag-length-value), auth plugin negotiation (two-round challenge/response vs.
single-round), result-set framing (column-count integer → column definitions →
EOF → rows → EOF vs. RowDescription → DataRow → CommandComplete), and
capability negotiation (bitmask exchange at handshake vs. always-on protocol
features).  If both extensions share the vtable without change, the vtable is
genuinely database-agnostic.

Gate E was confirmed by P6-2 (PostgreSQL extended query without state leaking
into the orchestrator).  P6-4 does **not** introduce a new gate; it is a second
data point showing that the vtable is not PostgreSQL-shaped.

---

## Deliverables

### 1. `extensions/mysql/mysql_packet.c` + `mysql_packet.h` — wire codec

MySQL packets share a common 4-byte header (3-byte LE length, 1-byte sequence
number).  All framing goes through two primitives:

```c
/* Write a 4-byte packet header into buf[0..3]. */
void mysql_write_pkt_header(uint8_t *buf, uint32_t payload_len, uint8_t seq);

/* Parse the 4-byte header; return payload length in *out_len and sequence
   number in *out_seq.  Returns 0 if fewer than 4 bytes are available. */
int mysql_read_pkt_header(const uint8_t *buf, size_t avail,
                          uint32_t *out_len, uint8_t *out_seq);
```

**Length-encoded integer (LEI) helpers** (used throughout result-set parsing):

```c
/* Decode a length-encoded integer at buf[0].
   Returns bytes consumed (1, 3, 4, or 9) or 0 if buf is too short.
   *out is set to UINT64_MAX for the NULL sentinel (0xfb). */
int mysql_read_lei(const uint8_t *buf, size_t avail, uint64_t *out);

/* Encode a length-encoded integer into buf.  Returns bytes written (1, 3, 4, 9)
   or 0 if cap is insufficient. */
int mysql_write_lei(uint8_t *buf, size_t cap, uint64_t val);
```

**Packets to encode:**

| Function | Purpose | Wire format |
|---|---|---|
| `mysql_encode_handshake_response(buf, cap, user, db, auth_resp, auth_resp_len, auth_plugin_name, client_flags)` | Login response | seq=1; Capabilities(4) + MaxPkt(4) + Charset(1) + filler(23) + user\0 + LEI(auth_len) + auth_data + db\0 + plugin_name\0 |
| `mysql_encode_com_query(buf, cap, sql, sql_len)` | Simple query | seq=0; payload: `0x03` + sql_bytes |
| `mysql_encode_com_quit(buf, cap)` | Graceful close | seq=0; payload: `0x01` |

`mysql_encode_handshake_response` is called once per connection during the
handshake; `mysql_encode_com_query` is called once per request.

`client_flags` for P6-4:

```c
#define MYSQL_CLIENT_LONG_PASSWORD  (1 << 0)
#define MYSQL_CLIENT_LONG_FLAG      (1 << 2)
#define MYSQL_CLIENT_CONNECT_WITH_DB (1 << 3)
#define MYSQL_CLIENT_PROTOCOL_41    (1 << 9)
#define MYSQL_CLIENT_SECURE_CONN    (1 << 15)
#define MYSQL_CLIENT_PLUGIN_AUTH    (1 << 19)

#define MYSQL_CLIENT_FLAGS_P64 \
    (MYSQL_CLIENT_LONG_PASSWORD | MYSQL_CLIENT_LONG_FLAG | \
     MYSQL_CLIENT_CONNECT_WITH_DB | MYSQL_CLIENT_PROTOCOL_41 | \
     MYSQL_CLIENT_SECURE_CONN | MYSQL_CLIENT_PLUGIN_AUTH)
```

**Packets to parse (`mysql_parse_packet`):**

```c
typedef enum {
    MYSQL_PKT_UNKNOWN = 0,
    MYSQL_PKT_HANDSHAKE_V10,   /* server greeting; seq=0             */
    MYSQL_PKT_AUTH_SWITCH_REQ, /* 0xfe + plugin name + challenge     */
    MYSQL_PKT_AUTH_MORE_DATA,  /* 0x01 + 1-byte marker (0x03/0x04)  */
    MYSQL_PKT_OK,              /* 0x00 or 0xfe (EOF alias in 4.1)    */
    MYSQL_PKT_EOF,             /* 0xfe + 2-byte warnings + 2-byte status */
    MYSQL_PKT_ERR,             /* 0xff + errno + sqlstate + message  */
    MYSQL_PKT_COLUMN_COUNT,    /* LEI > 0; result-set preamble       */
    MYSQL_PKT_COLUMN_DEF,      /* catalog / db / table / name fields */
    MYSQL_PKT_ROW,             /* raw row; one LEI string per column */
} mysql_pkt_type;

typedef struct mysql_parsed_pkt {
    mysql_pkt_type type;
    uint8_t        seq;
    union {
        struct {
            uint8_t  protocol_version;          /* must be 10        */
            char     server_version[32];         /* null-terminated   */
            uint32_t connection_id;
            uint8_t  auth_plugin_data[21];       /* 8 + up to 13 bytes */
            uint32_t server_capabilities;
            uint8_t  charset;
            uint16_t status_flags;
            char     auth_plugin_name[32];
        } handshake;
        struct {
            char     plugin_name[64];
            uint8_t  auth_data[21];
            uint8_t  auth_data_len;
        } auth_switch;
        struct {
            uint64_t affected_rows;
            uint64_t last_insert_id;
            uint16_t status_flags;
            uint16_t warnings;
        } ok;
        struct {
            uint16_t error_code;
            char     sqlstate[6];               /* 5 chars + NUL      */
            char     message[256];
        } err;
        struct {
            uint64_t count;
        } column_count;
        struct {
            char     schema[64];
            char     table[64];
            char     name[64];
            uint32_t type_oid;
        } column_def;
        struct {
            uint16_t warnings;
            uint16_t status_flags;
        } eof;
        struct {
            uint8_t marker;   /* 0x03 = fast-path success; 0x04 = full auth needed */
        } auth_more_data;
    };
} mysql_parsed_pkt;

/* Returns bytes consumed (>0), 0 if more data needed, -1 on parse error.
   context resolves packet-type ambiguities (MySQL result-set framing is
   position-dependent; 0x01 means AUTH_MORE_DATA in AUTH context but
   COLUMN_COUNT=1 in GENERIC context). */
typedef enum {
    MYSQL_CTX_AUTH,     /* connect() handshake loop: 0x01 → AUTH_MORE_DATA  */
    MYSQL_CTX_GENERIC,  /* readable() preamble: 0x01 → COLUMN_COUNT(1)      */
    MYSQL_CTX_COL_DEF,  /* column definition packets                         */
    MYSQL_CTX_ROW,      /* row data packets                                  */
} mysql_ctx;

int mysql_parse_packet(const uint8_t *buf, size_t avail,
                       mysql_ctx ctx, mysql_parsed_pkt *out);
```

**Auth helpers:**

```c
/* mysql_native_password: SHA1(password) XOR SHA1(challenge + SHA1(SHA1(password)))
   challenge is always exactly 20 bytes (auth_plugin_data from HandshakeV10). */
void mysql_native_password(const char *password, const uint8_t challenge[20],
                            uint8_t out[20]);

/* caching_sha2_password fast-path: SHA256(password) XOR SHA256(challenge + SHA256(SHA256(password)))
   challenge is always exactly 20 bytes (same auth_plugin_data nonce as native_password). */
void mysql_sha2_password_fast(const char *password, const uint8_t challenge[20],
                               uint8_t out[32]);
```

Both use OpenSSL (`EVP_MD_CTX`, `EVP_sha1()`, `EVP_sha256()`).  No new dependency.

`caching_sha2_password` full negotiation (SSL or RSA-encrypted key exchange) is
**out of scope for P6-4**.  If the server advertises `caching_sha2_password` and
is not in fast-path mode, the extension logs an error and fails the connection:

```
mysql: caching_sha2_password full exchange not supported in P6-4;
       configure server with mysql_native_password or use an already-cached user
```

---

### 2. `extensions/mysql/mysql.c` + `mysql.h` — protocol vtable

```c
static struct {
    struct addrinfo *addr;
    const char      *host;
    const char      *user;
    const char      *password;
    const char      *dbname;
} g_cfg;
```

URL parsing:
- `mysql://[user[:password]@]host[:port]/[dbname]`
- Default port: `3306`
- `user` defaults to `"wrkx"`; `dbname` defaults to `user`
- `mysql+tls://` is **out of scope for P6-4**
- Userinfo split follows the same `strchr(userinfo, ':')` pattern as the
  PostgreSQL extension; use `malloc+memcpy` (no `strndup`, C99)

**`mysql_configure_cb(wrkx_connect_info *)`** — populate `g_cfg`.

**Per-connection state:**

```c
#define MYSQL_RECVBUF 65536

typedef enum {
    MYSQL_PHASE_HANDSHAKE,  /* awaiting server greeting              */
    MYSQL_PHASE_AUTH,       /* sent HandshakeResponse, awaiting OK   */
    MYSQL_PHASE_AUTH_SWITCH,/* server requested plugin switch        */
    MYSQL_PHASE_READY,      /* OK received, idle                     */
    MYSQL_PHASE_QUERY,      /* COM_QUERY sent, reading result set    */
    MYSQL_PHASE_ERROR,      /* unrecoverable error                   */
} mysql_phase;

typedef enum {
    MYSQL_RS_PREAMBLE,      /* waiting for column count              */
    MYSQL_RS_COL_DEFS,      /* consuming column definition packets   */
    MYSQL_RS_ROWS,          /* consuming row packets                 */
} mysql_rs_phase;

typedef struct mysql_state {
    transport      xport;
    uint8_t        rbuf[MYSQL_RECVBUF];
    size_t         rbuf_len;
    mysql_phase    phase;
    mysql_rs_phase rs_phase;
    uint64_t       col_count;       /* columns expected in this result  */
    uint64_t       cols_seen;       /* column defs received so far      */
    int32_t        row_count;
    bool           done;
    bool           error;
    size_t         bytes;
    uint8_t        server_challenge[21]; /* 20 usable bytes + NUL     */
    char           auth_plugin[64];
} mysql_state;
```

**`connect(connection *c)`** — synchronous startup + auth:

```
1. calloc mysql_state; transport_init; transport_connect
2. sync_recv: read server HandshakeV10
   - extract auth_plugin_data (8 bytes from challenge1 + up to 13 from challenge2)
   - combine into s->server_challenge[0..19]; note s->auth_plugin name
3. if auth_plugin == "mysql_native_password" (or compatible):
       compute mysql_native_password(password, s->server_challenge, auth_resp)
   elif auth_plugin == "caching_sha2_password":
       compute mysql_sha2_password_fast(password, s->server_challenge, auth_resp)
   else:
       log "unsupported auth plugin: <name>"; goto fail
4. send mysql_encode_handshake_response(user, dbname, auth_resp, resp_len,
                                        s->auth_plugin, client_flags)
5. sync_recv loop (all calls use MYSQL_CTX_AUTH so 0x01 → AUTH_MORE_DATA):
   - OK                  → phase = MYSQL_PHASE_READY; break
   - AUTH_SWITCH_REQUEST → recompute auth_resp for new plugin+challenge;
                           send packet (seq = received_pkt.seq + 1, payload = auth_resp); loop
   - AUTH_MORE_DATA      → if marker == 0x03: loop (fast-path success; final OK follows)
                           if marker == 0x04: log "caching_sha2_password full exchange
                             not supported in P6-4; configure mysql_native_password or
                             use an already-cached user"; goto fail
   - ERR                 → log error.message; goto fail
   - 0xfe (status OK)   → treat as OK for Protocol 4.1; break
   - timeout 10000 ms   → goto fail
6. c->proto_state = s; return 0
```

The `AUTH_SWITCH_REQUEST` case (0xfe with plugin name + new challenge) supports
`mysql_native_password` ↔ `caching_sha2_password` fast-path switches.  A second
switch or an unknown target plugin causes a connection failure.

**`write(connection *c, buf, len)`** — forward bytes via `transport_write`; set
`phase = MYSQL_PHASE_QUERY`; reset `done`, `error`, `bytes`, `row_count`,
`cols_seen`, `col_count`; set `rs_phase = MYSQL_RS_PREAMBLE`.

**`readable(connection *c)`** — non-blocking, called from event loop:

```
1. transport_handshake (TLS guard, same as postgres)
2. transport_read into rbuf (append; shift consumed bytes each iteration)
3. parse loop:
   switch (s->rs_phase) {
   case MYSQL_RS_PREAMBLE:
       mysql_parse_packet(rbuf, rbuf_len, MYSQL_CTX_GENERIC, &pkt)
       → COLUMN_COUNT (n>0): s->col_count = n; s->rs_phase = MYSQL_RS_COL_DEFS; consume
       → OK:                 s->done = true; s->bytes += consumed; consume; break
       → ERR:                s->error = true; s->done = true; consume; break
       → 0 (incomplete):     break → PENDING
   case MYSQL_RS_COL_DEFS:
       mysql_parse_packet(rbuf, rbuf_len, MYSQL_CTX_COL_DEF, &pkt)
       → COLUMN_DEF:  s->cols_seen++; s->bytes += consumed; consume; continue
       → EOF:         if (s->cols_seen == s->col_count) rs_phase=MYSQL_RS_ROWS;
                      s->bytes += consumed; consume; continue
       → ERR:         s->error=true; s->done=true; consume; break
       → 0:           break → PENDING
   case MYSQL_RS_ROWS:
       mysql_parse_packet(rbuf, rbuf_len, MYSQL_CTX_ROW, &pkt)
       → ROW:         s->row_count++; s->bytes += consumed; consume; continue
       → EOF:         s->done=true; s->bytes += consumed; consume; break
       → ERR:         s->error=true; s->done=true; consume; break
       → 0:           break → PENDING
   }
4. if s->done: c->bytes = s->bytes; return s->error ? PROTO_DONE_STATUS_ERR : PROTO_DONE
5. return PROTO_PENDING
```

Context sensitivity in `mysql_parse_packet`: `MYSQL_CTX_ROW` treats any packet
whose first byte is not `0xfe` (EOF) and not `0xff` (ERR) as a raw row.  `COL_DEF`
parses the fixed-structure column definition packet.  `GENERIC` handles OK/ERR/EOF
and the initial column-count LEI.

**`close(connection *c)`** — send `COM_QUIT` (best-effort, ignore send errors);
`transport_close`; free `mysql_state`.

---

### 3. `extensions/mysql/mysql_lua_helpers.c` + `mysql_lua_helpers.h`

**`mysql.query(sql)`** — encodes a `COM_QUERY` packet and returns it as a Lua
string, exactly like `redis.command()` and `pg.query()`.

```c
static int lua_mysql_query(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "mysql.query: expected one string argument");

    size_t sql_len;
    const char *sql = lua_tolstring(L, 1, &sql_len);

    uint8_t buf[65540];   /* 4-byte header + up to 65535-byte payload */
    int n = mysql_encode_com_query(buf, sizeof(buf), sql, sql_len);
    if (n <= 0)
        return luaL_error(L, "mysql.query: SQL too large");

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}
```

Namespace: `"mysql@lua"` (following the `redis@lua`, `postgres@lua` convention).

No other helpers in P6-4.  `mysql.prepare()` / `mysql.execute()` (COM_STMT_*) are
P6-5.

---

### 4. `extensions/mysql/init.c` — extension entry point

```c
void wrkx_extension_init_mysql(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;
    api->register_protocol(mysql_protocol());
    api->register_helpers("mysql@lua",
                          mysql_lua_helpers, mysql_lua_helpers_count);
    api->register_schema("mysql", NULL, "3306", mysql_configure_cb);
}
```

`wrkx_extension.h` must **not** change.

---

### 5. `extensions/mysql/Makefile.ext` + `Makefile` integration

```makefile
# extensions/mysql/Makefile.ext
EXT_SRCS     += extensions/mysql/mysql.c \
                extensions/mysql/mysql_packet.c \
                extensions/mysql/mysql_lua_helpers.c \
                extensions/mysql/init.c
EXT_INIT_FNS += wrkx_extension_init_mysql
EXT_CFLAGS   += -Iextensions/mysql
```

Makefile additions (mirror the PostgreSQL codec + Lua test pattern):

```makefile
TEST_MY_CODEC_SRC := tests/unit/test_mysql_codec.c
TEST_MY_CODEC_BIN := obj/test_mysql_codec
MY_CODEC_DEPS     := extensions/mysql/mysql_packet.c

$(TEST_MY_CODEC_BIN): $(TEST_MY_CODEC_SRC) $(UNITY_SRC) $(MY_CODEC_DEPS) | $(ODIR)
	$(CC) $(CFLAGS) $(UNITY_INC) -Iextensions/mysql -Isrc \
	      -o $@ $(TEST_MY_CODEC_SRC) $(UNITY_SRC) $(MY_CODEC_DEPS) \
	      $(filter -L%,$(LIBS)) -lcrypto

TEST_MY_LUA_SRC    := tests/unit/test_mysql_lua.c
TEST_MY_LUA_BIN    := obj/test_mysql_lua
LUA_MY_ENGINE_DEPS := $(LUA_ENGINE_DEPS) \
                      extensions/mysql/mysql_packet.c \
                      extensions/mysql/mysql_lua_helpers.c \
                      extensions/mysql/mysql.c

$(TEST_MY_LUA_BIN): $(TEST_MY_LUA_SRC) $(UNITY_SRC) $(LUA_MY_ENGINE_DEPS) \
                    $(ODIR)/bytecode.o $(LDIR)/libluajit.a | $(ODIR)
	$(CC) $(CFLAGS) $(UNITY_INC) -Iextensions/mysql -Isrc \
	      -o $@ $(TEST_MY_LUA_SRC) $(UNITY_SRC) $(LUA_MY_ENGINE_DEPS) \
	      $(ODIR)/bytecode.o $(LDFLAGS) $(LIBS)
```

Both binaries added to `test-unit` prereqs; `test_mysql_lua` excluded from
`test-asan` (same LuaJIT policy as `test_mc_lua`, `test_pg_lua`).

---

### 6. `tests/unit/test_mysql_codec.c` — packet codec unit tests

Tests for `mysql_packet.c` in isolation (no TCP, no engine):

```
test_pkt_header_roundtrip
    mysql_write_pkt_header + mysql_read_pkt_header: length and seq survive

test_lei_decode_1byte
    0x05 → uint64=5, 1 byte consumed

test_lei_decode_2byte
    0xfc 0x00 0x01 → uint64=256, 3 bytes consumed

test_lei_decode_null
    0xfb → UINT64_MAX (NULL sentinel), 1 byte consumed

test_lei_encode_roundtrip
    values 0, 250, 251, 65535, 65536, 16777215, 16777216 → decode(encode(v)) == v

test_encode_com_query_format
    mysql_encode_com_query(buf, cap, "SELECT 1", 8) →
    header: payload_len=9, seq=0; payload[0]=0x03; rest = "SELECT 1"

test_encode_com_quit_format
    payload_len=1, seq=0, payload[0]=0x01

test_encode_handshake_response_structure
    user="wrkx", db="test", auth_plugin_name="mysql_native_password", auth_resp=20-byte buf →
    header seq=1; capabilities at offset 4; user field null-terminated;
    auth_resp LEI-prefixed; db\0 present after auth data;
    "mysql_native_password\0" present and NUL-terminated after db\0

test_parse_auth_more_data_fast_success
    0x01 0x03 payload in MYSQL_CTX_AUTH →
    MYSQL_PKT_AUTH_MORE_DATA, marker=0x03

test_parse_auth_more_data_full_auth
    0x01 0x04 payload in MYSQL_CTX_AUTH →
    MYSQL_PKT_AUTH_MORE_DATA, marker=0x04

test_parse_column_count_preamble_single
    0x01 (LEI = 1) in MYSQL_CTX_GENERIC →
    MYSQL_PKT_COLUMN_COUNT, count=1  (not AUTH_MORE_DATA; context resolves the ambiguity)

test_parse_ok_packet
    construct a minimal OK packet (0x00 + LEI(0) + LEI(0) + status(2) + warnings(2)) →
    MYSQL_PKT_OK, affected_rows=0, last_insert_id=0

test_parse_err_packet
    construct ERR (0xff + errno_le16 + '#' + "HY000" + "error text") →
    MYSQL_PKT_ERR, error_code correct, sqlstate="HY000", message="error text"

test_parse_eof_packet
    0xfe + warnings(2) + status(2) → MYSQL_PKT_EOF, warnings correct

test_parse_column_count_preamble
    0x03 (LEI = 3) in MYSQL_CTX_GENERIC → MYSQL_PKT_COLUMN_COUNT, count=3

test_parse_column_def
    construct a minimal column definition (catalog/db/table/org_table/name/org_name
    all "\x00\0" length-prefixed, charset 0x08, type 0xfd) in MYSQL_CTX_COL_DEF →
    MYSQL_PKT_COLUMN_DEF, name field parseable

test_parse_row_generic
    arbitrary 3-byte payload in MYSQL_CTX_ROW (not 0xfe/0xff) → MYSQL_PKT_ROW

test_parse_incomplete_returns_zero
    truncated packet at every byte boundary → returns 0, no crash

test_native_password_known_vector
    password="secret", challenge=20 known bytes →
    expected 20-byte SHA1 digest verified against an offline-computed reference

test_sha2_fast_path_known_vector
    password="secret", challenge=20 known bytes →
    expected 32-byte SHA256 digest against known reference
```

---

### 7. `tests/unit/test_mysql_lua.c` — Lua helper unit tests

```
test_mysql_query_select1
    mysql.query("SELECT 1") → Lua string; byte[4]==0x03 (COM_QUERY); rest=="SELECT 1"

test_mysql_query_wrong_type_errors
    mysql.query(42) → Lua error, no crash

test_mysql_query_empty_sql
    mysql.query("") → valid 5-byte packet (header + 0x03 + nothing)

test_mysql_query_large_sql
    mysql.query(string.rep("x", 16384)) → succeeds, correct length in header

test_mysql_query_too_large_errors
    mysql.query(string.rep("x", 65536)) → Lua error ("SQL too large")
```

---

### 8. `scripts/mysql_select.lua` — example workload

```lua
-- scripts/mysql_select.lua
--
-- Basic MySQL workload for wrkx (ADR 0005, P6-4).
-- Issues a simple SELECT against a counter value.
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/mysql_select.lua \
--          mysql://wrkx:secret@localhost/wrkx

local counter = 0

function request()
    counter = counter + 1
    local key = counter % 100
    return mysql.query(string.format("SELECT %d", key))
end
```

---

### 9. `tests/e2e/mysql_basic.sh` — E2E test

Requires `MYSQL_URL` env var (skip cleanly if unset).

```
PASS  mysql extension builds (make EXTENSIONS="redis memcached postgres mysql")
PASS  connection smoke test: -t1 -c1 -d2s -R10 SELECT 1 exits 0, > 0 requests
PASS  throughput smoke test: -t1 -c4 -d5s -R100 reports > 0 requests, 0 errors
PASS  error query (syntax error SQL) reports non-zero error count, does not crash
PASS  wrong password returns connection error within 10s, not hang
PASS  no core engine changes since HEAD (frozen-file diff clean)
```

Frozen paths for diff check (same list as PostgreSQL E2E tests plus
`extensions/postgres/`):

```bash
FROZEN="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c \
        src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h \
        src/net.c src/net.h \
        src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h \
        extensions/postgres/postgres.c extensions/postgres/postgres.h \
        extensions/postgres/pg_message.c extensions/postgres/pg_message.h"
```

CI matrix addition: a `mysql` service container (e.g. `mysql:8.0`) with
`MYSQL_ROOT_PASSWORD`, `MYSQL_USER=wrkx`, `MYSQL_PASSWORD=secret`,
`MYSQL_DATABASE=wrkx`; healthcheck via `mysqladmin ping`.

---

## Guards

1. `make test` — `test_mysql_codec` and `test_mysql_lua` pass; all existing
   tests still pass; no regressions in `test_pg_codec`, `test_mc_codec`, etc.
2. `make EXTENSIONS="redis memcached postgres mysql" test-asan` — clean under
   ASAN; no memory leaks in codec or connection lifecycle; `test_mysql_lua`
   excluded from ASAN per LuaJIT policy
3. `MYSQL_URL=mysql://wrkx:secret@localhost/wrkx make EXTENSIONS="redis memcached postgres mysql" test`
   — `tests/e2e/mysql_basic.sh` passes; test skips cleanly if `MYSQL_URL` is
   absent; fails with clear "unknown schema" error if mysql extension not compiled in
4. Frozen-file diff is empty: `src/orchestrator.*`, `src/ae.*`, `src/rate.*`,
   `src/net.*`, `src/transport.*`, `include/wrkx_extension.h`,
   `include/wrkx_transport.h`, `extensions/redis/`, `extensions/postgres/`
5. `scripts/adr-compliance.sh` passes (no private core header includes from
   `extensions/mysql/`)

## Core engine touch

Zero.  All new code is within `extensions/mysql/`, `tests/unit/`,
`tests/e2e/`, and `scripts/`.

`wrkx_extension.h` must **not** change.  If the MySQL extension reveals a gap
in the extension API, record it and address it in a separate task before P6-5.

## Out of scope for P6-4

- TLS (`mysql+tls://`) — requires transport-level TLS negotiation after the
  initial plaintext server greeting; defer to P6-5 or a dedicated task
- `caching_sha2_password` full exchange (RSA key + encrypted password or SSL
  path) — defer to P6-5
- `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` — prepared statements are P6-5
- Result-set field decoding / Lua result objects — defer; P6-4 counts rows
  and accumulates bytes only, same as P6-1
- QuickJS helpers (`mysql@quickjs`) — follow-on after P6-4
- Connection pool reset / `COM_RESET_CONNECTION` — P6-5 scope
- Binary result-set protocol (flag `CLIENT_DEPRECATE_EOF` and binary row
  format) — not needed to confirm the architecture
