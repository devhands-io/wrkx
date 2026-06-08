title: mysql extension — P6-5 prepared statements (COM_STMT_*, text params, QuickJS helpers)
status: completed
adr: 0005
adr-step: P6-5
depends: t085

## Why / Goal

P6-5 adds prepared-statement support to the MySQL extension and adds the QuickJS
helpers (mysql@quickjs) that were deferred from P6-4.  It is **not** a new
architectural gate — Gate E was confirmed by P6-2 (PostgreSQL extended query).
This is feature completion: real MySQL workloads use prepared statements for
server-side parse caching and binary parameter binding.

The MySQL prepared-statement flow is architecturally distinct from PostgreSQL's
in one important way.  PostgreSQL's `pg.execute()` bundles Parse+Bind+Execute+Sync
into a single TCP send; the server processes them as a pipeline.  MySQL's
COM_STMT_PREPARE requires a full roundtrip — the server returns a `stmt_id` in
COM_STMT_PREPARE_OK, and only then can COM_STMT_EXECUTE be formed.  There is no
way to pipeline these.  Consequently, the protocol vtable's `readable()` must
send COM_STMT_EXECUTE after receiving COM_STMT_PREPARE_OK.  Writing from
`readable()` is already established (the auth handshake in `connect()` uses
synchronous read/write loops; `transport_handshake()` inside `readable()` also
writes).  This task extends that precedent to the event-loop path.

---

## Deliverables

### 1. Internal "prepared execute" blob format

`mysql.execute()` cannot produce raw MySQL wire bytes because the stmt_id is not
known until the server responds to COM_STMT_PREPARE.  Instead it produces an
internal blob that `mysql_write()` knows how to expand.

**Magic sentinel:** `0xFF 0xFF 0xFF 0xEE`

This is distinguishable from any real MySQL client packet: a real packet's first
three bytes are the payload length (LE), and seq (byte 3) is always 0x00 for the
first send of a request.  Byte 3 = 0xEE is therefore unambiguous.

**Practical size limit: 1,023 bytes of SQL.**  Both scripting helpers and
`mysql_write()` reject SQL longer than `MYSQL_MAX_PREPARED_SQL` (1,023) bytes
(`sql_len > MYSQL_MAX_PREPARED_SQL`), because the SQL must fit the per-connection
cache key.  This is the binding constraint — not the theoretical sentinel limit.
The rejection must be tested explicitly (see §7).

**Blob layout:**

```
[0..3]   0xFF 0xFF 0xFF 0xEE   (magic)
[4..7]   sql_len  (uint32 LE)
[8..]    sql bytes (sql_len bytes)
[8+sql_len]    n_params (uint8)
for each param i in [0 .. n_params-1]:
    [0]        type  (0x00 = NULL, 0x01 = string)
    if type == 0x01:
        [1..4] value_len (uint32 LE)
        [5..]  value bytes
```

The blob is produced by a new codec helper and consumed exclusively by
`mysql_write()`.  It never appears on the wire.

---

### 2. `extensions/mysql/mysql_packet.c` + `mysql_packet.h` — codec additions

**New constant in `mysql_packet.h`:**

```c
/* Maximum SQL length for a prepared statement.  The per-connection stmt cache
   stores the SQL in a fixed buffer of this size plus a NUL terminator.
   Enforced by both the scripting helpers (Lua/QuickJS) and mysql_write(). */
#define MYSQL_MAX_PREPARED_SQL 1023
```

**New packet types:**

```c
typedef enum {
    ...  /* existing */
    MYSQL_PKT_STMT_PREPARE_OK,  /* 0x00 in STMT_PREPARE response context     */
    MYSQL_PKT_BINARY_ROW,       /* COM_STMT_EXECUTE result row                */
} mysql_pkt_type;
```

**New parse context:**

```c
typedef enum {
    MYSQL_CTX_AUTH,
    MYSQL_CTX_GENERIC,
    MYSQL_CTX_COL_DEF,
    MYSQL_CTX_ROW,
    MYSQL_CTX_STMT_PREPARE,   /* resolves 0x00 → STMT_PREPARE_OK             */
    MYSQL_CTX_BINARY_ROW,     /* resolves non-0xfe/0xff → BINARY_ROW         */
} mysql_ctx;
```

**New union member in `mysql_parsed_pkt`:**

```c
struct {
    uint32_t stmt_id;
    uint16_t n_columns;
    uint16_t n_params;
    uint16_t warning_count;
} stmt_prepare_ok;
```

**New encoder functions** (all return bytes written > 0, or 0 if buf too small):

```c
/* COM_STMT_PREPARE (seq=0, cmd=0x16). */
int mysql_encode_com_stmt_prepare(uint8_t *buf, size_t cap,
                                  const char *sql, size_t sql_len);

/* COM_STMT_EXECUTE (seq=0, cmd=0x17).
 * Text-mode params only (all MYSQL_TYPE_VAR_STRING / 0x00fd).
 * params[i] == NULL → NULL param; param_lens[i] is ignored for NULL.
 * Returns 0 if buffer is too small or n_params > 127. */
int mysql_encode_com_stmt_execute(uint8_t *buf, size_t cap,
                                  uint32_t stmt_id,
                                  const char **params,
                                  const size_t *param_lens,
                                  int n_params);

/* COM_STMT_CLOSE (seq=0, cmd=0x19, no server response). */
int mysql_encode_com_stmt_close(uint8_t *buf, size_t cap, uint32_t stmt_id);

/* Produce the internal "prepared execute" blob (not MySQL wire).
 * Same param conventions as mysql_encode_com_stmt_execute. */
int mysql_encode_prepared_request(uint8_t *buf, size_t cap,
                                  const char *sql, size_t sql_len,
                                  const char **params,
                                  const size_t *param_lens,
                                  int n_params);

/* Returns 1 if buf[0..3] == the prepared-request magic, 0 otherwise. */
int mysql_is_prepared_request(const uint8_t *buf, size_t len);
```

**COM_STMT_EXECUTE wire format** (text-mode):

```
header (4 bytes): payload_len(3 LE) + seq=0x00
payload:
  [0]      0x17   (COM_STMT_EXECUTE)
  [1..4]   stmt_id (uint32 LE)
  [5]      0x00   (cursor type: no cursor)
  [6..9]   0x01 0x00 0x00 0x00   (iteration-count = 1)
  if n_params > 0:
    [10..]  null_bitmap  (ceil(n_params / 8) bytes)
            new_params_bound_flag = 0x01
            for each param: 2 bytes — type_byte=0xfd (MYSQL_TYPE_VAR_STRING)
                                       + unsigned_flag_byte=0x00
            for each non-null param: LEI(value_len) + value_bytes
```

**COM_STMT_PREPARE_OK wire format** (parsed in MYSQL_CTX_STMT_PREPARE):

```
header (4 bytes)
payload:
  [0]      0x00   (OK marker)
  [1..4]   stmt_id  (uint32 LE)
  [5..6]   n_columns (uint16 LE)
  [7..8]   n_params  (uint16 LE)
  [9]      0x00   (reserved)
  [10..11] warning_count (uint16 LE)
```

After the prepare-OK packet, if `n_params > 0`: `n_params` column-def packets
followed by one EOF packet.  If `n_columns > 0`: `n_columns` column-def packets
followed by one EOF packet.  `mysql_parse_packet` with `MYSQL_CTX_COL_DEF`
handles both.

---

### 3. `extensions/mysql/mysql.c` — state machine additions

**New phases:**

```c
typedef enum {
    MYSQL_PHASE_HANDSHAKE,
    MYSQL_PHASE_AUTH,
    MYSQL_PHASE_AUTH_SWITCH,
    MYSQL_PHASE_READY,
    MYSQL_PHASE_QUERY,          /* COM_QUERY in flight                       */
    MYSQL_PHASE_PREPARING,      /* COM_STMT_PREPARE sent; awaiting OK        */
    MYSQL_PHASE_STMT_PARAM_DEFS,/* consuming n_params column-def + EOF       */
    MYSQL_PHASE_STMT_COL_DEFS,  /* consuming n_columns column-def + EOF      */
    MYSQL_PHASE_STMT_EXECUTING, /* COM_STMT_EXECUTE sent; reading result     */
    MYSQL_PHASE_ERROR,
} mysql_phase;
```

**New fields in `mysql_state`:**

```c
/* Prepared-statement cache: one slot per connection. */
uint32_t stmt_id;             /* 0 = no statement prepared                  */
uint16_t stmt_n_params;       /* param count from COM_STMT_PREPARE_OK       */
uint16_t stmt_n_columns;      /* column count from COM_STMT_PREPARE_OK      */
uint16_t stmt_meta_remaining; /* column/param defs left to consume          */
char     stmt_sql[1024];      /* SQL of prepared statement (for cache key)  */

/* Pending execute params stashed during PREPARING phase. */
uint8_t  pending[65540];      /* copy of the internal prepared-execute blob */
size_t   pending_len;
```

**`mysql_write()` additions:**

```c
if (mysql_is_prepared_request((const uint8_t *)buf, len)) {
    /* decode blob: extract sql, sql_len, params, n_params */

    bool same_stmt = (s->stmt_id != 0 &&
                      sql_len < sizeof(s->stmt_sql) &&
                      memcmp(sql, s->stmt_sql, sql_len) == 0 &&
                      s->stmt_sql[sql_len] == '\0');

    if (same_stmt) {
        /* fast path: stmt already prepared, send execute directly */
        /* validate param count against what the server reported in PREPARE_OK */
        if (n_params != (int)s->stmt_n_params) return -1;
        uint8_t ebuf[65540];
        int n = mysql_encode_com_stmt_execute(ebuf, sizeof(ebuf),
                                              s->stmt_id, params,
                                              param_lens, n_params);
        if (n <= 0) return -1;
        size_t written;
        if (transport_write(&s->xport, (char *)ebuf, (size_t)n, &written)
                != TRANSPORT_OK) return -1;
        if (written != (size_t)n) return -1;  /* short write = error */
        s->phase     = MYSQL_PHASE_STMT_EXECUTING;
        s->rs_phase  = MYSQL_RS_PREAMBLE;
        s->done      = false;
        s->error     = false;
        s->bytes     = 0;
        s->row_count = 0;
        s->col_count = 0;
        s->cols_seen = 0;
        return 0;
    }

    /* validate before touching connection state — all checks must pass before
       we send COM_STMT_CLOSE; closing the cached stmt and then returning -1
       would leave the connection with stmt_id=0 and no prepare pending. */
    if (len > sizeof(s->pending)) return -1;
    if (sql_len > MYSQL_MAX_PREPARED_SQL) return -1;  /* cache-key limit */

    /* close existing stmt before re-preparing a different SQL.
       Unlike mysql_close() where the connection is being torn down, this
       close happens mid-stream: a failed or short write here corrupts the
       packet sequence and the following COM_STMT_PREPARE would append to
       garbage.  Treat any failure as fatal. */
    if (s->stmt_id != 0) {
        uint8_t cbuf[16];
        int cn = mysql_encode_com_stmt_close(cbuf, sizeof(cbuf), s->stmt_id);
        if (cn <= 0) return -1;
        size_t cw;
        if (transport_write(&s->xport, (char *)cbuf, (size_t)cn, &cw)
                != TRANSPORT_OK) return -1;
        if (cw != (size_t)cn) return -1;  /* short write = error */
        s->stmt_id = 0;
    }

    /* stash blob for use once PREPARE_OK arrives */
    memcpy(s->pending, buf, len);
    s->pending_len = len;

    /* copy sql into cache key (NUL-terminate) */
    memcpy(s->stmt_sql, sql, sql_len);
    s->stmt_sql[sql_len] = '\0';

    /* send COM_STMT_PREPARE */
    uint8_t pbuf[65540];
    int pn = mysql_encode_com_stmt_prepare(pbuf, sizeof(pbuf), sql, sql_len);
    if (pn <= 0) return -1;
    size_t pw;
    if (transport_write(&s->xport, (char *)pbuf, (size_t)pn, &pw)
            != TRANSPORT_OK) return -1;
    if (pw != (size_t)pn) return -1;  /* short write = error */

    s->phase    = MYSQL_PHASE_PREPARING;
    s->done     = false;
    s->error    = false;
    s->bytes    = 0;
    s->row_count = 0;
    return 0;
}
/* else: fall through to existing COM_QUERY path */
```

**`mysql_readable()` restructuring — top-level phase dispatch:**

The current implementation dispatches on `s->rs_phase` alone inside a `for(;;)`
parse loop.  P6-5 requires a top-level `switch (s->phase)` that wraps that loop,
because the new PREPARING/STMT_* phases require a different parse context and
cannot share the `rs_phase` sub-state machine entry point.  Without this, a
COM_STMT_PREPARE_OK arriving while `rs_phase == MYSQL_RS_PREAMBLE` would be
parsed under `MYSQL_CTX_GENERIC`, misidentified as a plain OK, and
COM_STMT_EXECUTE would never be sent.

Required structure:

```c
static proto_status mysql_readable(connection *c) {
    mysql_state *s = (mysql_state *)c->proto_state;

    /* TLS guard */
    transport_status ts = transport_handshake(&s->xport);
    if (ts == TRANSPORT_RETRY) return PROTO_PENDING;
    if (ts == TRANSPORT_ERROR) return PROTO_ERROR;

    /* Append incoming bytes */
    size_t n;
    ts = transport_read(&s->xport, s->rbuf + s->rbuf_len,
                        MYSQL_RECVBUF - s->rbuf_len, &n);
    if (ts == TRANSPORT_RETRY) return PROTO_PENDING;
    if (ts == TRANSPORT_EOF || ts == TRANSPORT_ERROR) return PROTO_ERROR;
    s->rbuf_len += n;

    /* Outer loop: re-dispatches immediately when a phase transition leaves
       data in rbuf.  Without this, a phase change followed by goto pending
       returns PROTO_PENDING while buffered metadata packets sit in rbuf
       unprocessed — no new fd event will fire to drain them, causing a stall.
       Each case breaks or continues the outer loop; only goto pending / done
       exits the function. */
    for (;;) {  /* outer: re-dispatch after phase transitions */
    switch (s->phase) {

    case MYSQL_PHASE_QUERY:
    case MYSQL_PHASE_STMT_EXECUTING:
        /* Enter the result-set sub-state machine.
           STMT_EXECUTING uses MYSQL_CTX_BINARY_ROW for row packets;
           QUERY uses MYSQL_CTX_ROW.  All other rs_phase contexts are shared. */
        for (;;) { /* existing rs_phase loop, parameterised on row context */ }
        break;

    case MYSQL_PHASE_PREPARING: {
        /* PREPARE_OK is a single packet; no inner loop needed. */
        mysql_parsed_pkt pkt; int rc; size_t consumed;
        rc = mysql_parse_packet(s->rbuf, s->rbuf_len,
                                MYSQL_CTX_STMT_PREPARE, &pkt);
        if (rc == 0) goto pending;
        if (rc < 0)  return PROTO_ERROR;
        consumed = (size_t)rc;
        s->bytes += consumed;
        s->rbuf_len -= consumed;
        memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
        if (pkt.type == MYSQL_PKT_STMT_PREPARE_OK) {
            s->stmt_id        = pkt.stmt_prepare_ok.stmt_id;
            s->stmt_n_params  = pkt.stmt_prepare_ok.n_params;
            s->stmt_n_columns = pkt.stmt_prepare_ok.n_columns;
            if (s->stmt_n_params > 0) {
                s->stmt_meta_remaining = s->stmt_n_params;
                s->phase = MYSQL_PHASE_STMT_PARAM_DEFS;
            } else if (s->stmt_n_columns > 0) {
                s->stmt_meta_remaining = s->stmt_n_columns;
                s->phase = MYSQL_PHASE_STMT_COL_DEFS;
            } else {
                goto send_execute;
            }
            continue;  /* re-dispatch into new phase; don't wait for next fd event */
        } else if (pkt.type == MYSQL_PKT_ERR) {
            s->error = true; s->done = true;
            goto done;
        } else {
            return PROTO_ERROR;
        }
        break;
    }

    case MYSQL_PHASE_STMT_PARAM_DEFS:
    case MYSQL_PHASE_STMT_COL_DEFS:
        for (;;) {
            mysql_parsed_pkt pkt; int rc; size_t consumed;
            rc = mysql_parse_packet(s->rbuf, s->rbuf_len,
                                    MYSQL_CTX_COL_DEF, &pkt);
            if (rc == 0) goto pending;
            if (rc < 0)  return PROTO_ERROR;
            consumed = (size_t)rc;
            s->bytes += consumed;
            s->rbuf_len -= consumed;
            memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
            if (pkt.type == MYSQL_PKT_COLUMN_DEF) {
                if (s->stmt_meta_remaining > 0) s->stmt_meta_remaining--;
            } else if (pkt.type == MYSQL_PKT_EOF) {
                if (s->phase == MYSQL_PHASE_STMT_PARAM_DEFS) {
                    if (s->stmt_n_columns > 0) {
                        s->stmt_meta_remaining = s->stmt_n_columns;
                        s->phase = MYSQL_PHASE_STMT_COL_DEFS;
                        break;  /* break inner; continue outer to re-dispatch */
                    } else {
                        goto send_execute;
                    }
                } else {
                    goto send_execute;
                }
            } else if (pkt.type == MYSQL_PKT_ERR) {
                s->error = true; s->done = true;
                goto done;
            }
        }
        continue;  /* re-dispatch: PARAM_DEFS→COL_DEFS transition with rbuf data */

    default:
        return PROTO_ERROR;
    }
    break;  /* no phase change occurred; exit outer loop */
    }  /* end outer for(;;) */
    /* ... */
}
```

The existing `rs_phase` sub-state machine (PREAMBLE → COL_DEFS → ROWS) is
preserved intact.  `MYSQL_PHASE_STMT_EXECUTING` enters it with row context
`MYSQL_CTX_BINARY_ROW`; `MYSQL_PHASE_QUERY` enters it with `MYSQL_CTX_ROW`.
A simple way to implement this: factor the row-context choice into a local
variable set at the top of the QUERY/STMT_EXECUTING case, then pass it into
`mysql_parse_packet` for `MYSQL_RS_ROWS`.  No other changes to the rs_phase
logic are needed.

`send_execute` label (shared by PREPARING, STMT_PARAM_DEFS, STMT_COL_DEFS after
consuming all metadata):

```c
send_execute: {
    /* decode params from stashed pending blob */
    const char *params[128]; size_t param_lens[128]; int n_params;
    if (mysql_decode_prepared_request_params(
            s->pending, s->pending_len, params, param_lens, &n_params) < 0)
        return PROTO_ERROR;

    /* validate param count against what the server reported in PREPARE_OK */
    if (n_params != (int)s->stmt_n_params) return PROTO_ERROR;

    uint8_t ebuf[65540];
    int en = mysql_encode_com_stmt_execute(ebuf, sizeof(ebuf),
                                           s->stmt_id, params,
                                           param_lens, n_params);
    if (en <= 0) return PROTO_ERROR;
    size_t ew;
    if (transport_write(&s->xport, (char *)ebuf, (size_t)en, &ew)
            != TRANSPORT_OK) return PROTO_ERROR;
    if (ew != (size_t)en) return PROTO_ERROR;  /* short write = error */

    s->phase    = MYSQL_PHASE_STMT_EXECUTING;
    s->rs_phase = MYSQL_RS_PREAMBLE;
    s->col_count = 0; s->cols_seen = 0; s->row_count = 0;
    goto pending;  /* re-enter the read loop on next readable() call */
}
```

A new codec helper is needed:

```c
/* Decode params from a prepared-request blob (internal format).
   Fills params[], param_lens[], *n_params.  params[i]==NULL for NULL entries.
   String values point into blob memory (zero-copy).
   Returns 0 on success, -1 if blob is malformed. */
int mysql_decode_prepared_request_params(const uint8_t *blob, size_t blob_len,
                                         const char **params,
                                         size_t *param_lens, int *n_params);
```

**`mysql_close()` change:** if `stmt_id != 0`, send COM_STMT_CLOSE before COM_QUIT.

---

### 4. `extensions/mysql/mysql_lua_helpers.c/.h` — additions

**`mysql.prepare(sql)`** — unchanged in signature, already exists.  If not present
(P6-4 stub only returned `{sql=sql}`), confirm it exists and is correct.

**`mysql.execute(handle_or_sql, param1, ...)`** — new helper:

```c
static int lua_mysql_execute(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    int nargs = lua_gettop(L);
    if (nargs < 1)
        return luaL_error(L, "mysql.execute: expected sql or mysql.prepare() handle");

    const char *sql = NULL;
    size_t      sql_len = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        sql = lua_tolstring(L, 1, &sql_len);
    } else if (lua_type(L, 1) == LUA_TTABLE) {
        lua_getfield(L, 1, "sql");
        if (lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "mysql.execute: invalid mysql.prepare() handle");
        sql = lua_tolstring(L, -1, &sql_len);
        lua_pop(L, 1);
    } else {
        return luaL_error(L, "mysql.execute: first arg must be SQL string or handle");
    }

    /* enforce cache-key limit here so Lua sees an error rather than having
       mysql_write() silently reject the blob later */
    if (sql_len > MYSQL_MAX_PREPARED_SQL)
        return luaL_error(L, "mysql.execute: SQL exceeds %d-byte limit",
                          MYSQL_MAX_PREPARED_SQL);

    int n_params = nargs - 1;
    if (n_params > 127)
        return luaL_error(L, "mysql.execute: too many parameters (max 127)");

    const char *params[128]; size_t param_lens[128];
    for (int i = 0; i < n_params; i++) {
        int idx = i + 2;
        if (lua_isnil(L, idx)) {
            params[i] = NULL; param_lens[i] = 0;
        } else if (lua_type(L, idx) == LUA_TSTRING ||
                   lua_type(L, idx) == LUA_TNUMBER) {
            params[i] = lua_tolstring(L, idx, &param_lens[i]);
        } else {
            return luaL_error(L,
                "mysql.execute: param %d must be string, number, or nil", i + 1);
        }
    }

    uint8_t buf[65540];   /* must match sizeof(s->pending) and ebuf in mysql.c */
    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          sql, sql_len,
                                          params, param_lens, n_params);
    if (n <= 0) return luaL_error(L, "mysql.execute: SQL or params too large");

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}
```

Register as `"execute"` under `"mysql@lua"` namespace alongside `"query"` and
`"prepare"`.

---

### 5. `extensions/mysql/mysql_quickjs_helpers.c` + `mysql_quickjs_helpers.h` — new files

Mirrors `pg_quickjs_helpers.c` exactly in structure.  Provides `mysql@quickjs`
namespace with three helpers:

- **`mysql.query(sql)`** — same logic as `lua_mysql_query`, using QuickJS types
- **`mysql.prepare(sql)`** — returns `{sql: sql}` JS object
- **`mysql.execute(handle_or_sql, ...)`** — same logic as `lua_mysql_execute`,
  using `JS_ToCStringLen` / `JS_IsString` / `JS_IsObject` patterns from
  `pg_quickjs_helpers.c`; must check `sql_len > MYSQL_MAX_PREPARED_SQL` and
  throw a JS TypeError before calling the encoder, exactly as the Lua helper does

Protected by `#ifdef WRKX_HAVE_QUICKJS`.

**`extensions/mysql/init.c` addition:**

```c
#ifdef WRKX_HAVE_QUICKJS
    api->register_helpers("mysql@quickjs",
                          mysql_quickjs_helpers, mysql_quickjs_helpers_count);
#endif
```

---

### 6. `extensions/mysql/Makefile.ext` — additions

```makefile
EXT_SRCS += extensions/mysql/mysql_quickjs_helpers.c
```

`test_mysql_lua` Makefile target gains `extensions/mysql/mysql_quickjs_helpers.c`
only if `WRKX_HAVE_QUICKJS` is set, mirroring the postgres pattern.

New test target `test_mysql_stmt_codec` (see §7).

---

### 7. `tests/unit/test_mysql_codec.c` — additions

New tests appended to the existing file:

```
test_encode_com_stmt_prepare_format
    mysql_encode_com_stmt_prepare(buf, cap, "SELECT ?", 8) →
    header: seq=0, payload_len=9; payload[0]=0x17 [wait — it's 0x16 for prepare];
    [4]=0x16; rest = "SELECT ?"

test_encode_com_stmt_close_format
    mysql_encode_com_stmt_close(buf, cap, 42) →
    header: seq=0, payload_len=5; payload[0]=0x19;
    stmt_id in [5..8] = 42 LE

test_encode_com_stmt_execute_no_params
    mysql_encode_com_stmt_execute(buf, cap, 7, NULL, NULL, 0) →
    header seq=0; payload[0]=0x17; stmt_id=7 at [1..4]; cursor=0x00; iter=1 LE;
    no null bitmap section (n_params=0); no new_params_bound_flag

test_encode_com_stmt_execute_string_param
    params={"hello"}, param_lens={5}, n_params=1 →
    null_bitmap=0x00 (not null); new_params_bound_flag=0x01;
    type entry (2 bytes): 0xfd 0x00 (MYSQL_TYPE_VAR_STRING + unsigned_flag);
    value: LEI(5) + "hello"

test_encode_com_stmt_execute_null_param
    params={NULL}, param_lens={0}, n_params=1 →
    null_bitmap=0x01 (bit 0 set); new_params_bound_flag=0x01;
    type entry (2 bytes): 0xfd 0x00; no value bytes

test_encode_com_stmt_execute_two_mixed_params
    params={"foo", NULL}, param_lens={3, 0} →
    null_bitmap=0x02 (bit 1 set for second param); two type entries

test_parse_stmt_prepare_ok
    construct a minimal COM_STMT_PREPARE_OK payload (0x00 + stmt_id=5 LE +
    n_columns=1 LE + n_params=1 LE + 0x00 reserved + warnings=0 LE);
    mysql_parse_packet(..., MYSQL_CTX_STMT_PREPARE, &pkt) →
    MYSQL_PKT_STMT_PREPARE_OK, stmt_id=5, n_columns=1, n_params=1

test_parse_binary_row_generic
    arbitrary 3-byte payload (not 0xfe/0xff) in MYSQL_CTX_BINARY_ROW →
    MYSQL_PKT_BINARY_ROW

test_encode_prepared_request_roundtrip
    mysql_encode_prepared_request("SELECT ?", 8, params={"val"}, lens={3}, 1) →
    magic bytes present; mysql_decode_prepared_request_params → sql matches,
    n_params=1, params[0]="val"

test_encode_prepared_request_null_param_roundtrip
    null param → decode → params[0]==NULL

test_encode_prepared_request_no_params
    n_params=0 → decode → n_params=0

test_is_prepared_request_true
    blob from mysql_encode_prepared_request → mysql_is_prepared_request returns 1

test_is_prepared_request_false_com_query
    real COM_QUERY packet → mysql_is_prepared_request returns 0
```

---

### 8. `tests/unit/test_mysql_lua.c` — additions

```
test_mysql_execute_no_params
    mysql.execute("SELECT 1") → blob; mysql_is_prepared_request → true;
    decode: sql="SELECT 1", n_params=0

test_mysql_execute_with_string_param
    mysql.execute("SELECT ?", "hello") → blob; decode: n_params=1, params[0]="hello"

test_mysql_execute_with_number_param
    mysql.execute("SELECT ?", 42) → blob; decode: n_params=1,
    params[0] is the string "42" (lua_tolstring converts)

test_mysql_execute_with_null_param
    mysql.execute("SELECT ?", nil) → blob; decode: n_params=1, params[0]==NULL

test_mysql_execute_from_prepare_handle
    local h = mysql.prepare("SELECT ?")
    mysql.execute(h, "x") → blob; decode: sql="SELECT ?", params[0]="x"

test_mysql_execute_wrong_type_errors
    mysql.execute(42) → Lua error, no crash

test_mysql_execute_too_many_params
    mysql.execute("SELECT 1", string.rep("x", 1) × 128) → Lua error

test_mysql_execute_sql_at_limit
    mysql.execute(string.rep("x", 1023)) → succeeds (exactly at the 1023-byte limit)

test_mysql_execute_sql_over_limit
    mysql.execute(string.rep("x", 1024)) → Lua error matching "SQL exceeds.*limit"

test_mysql_prepare_still_works
    mysql.prepare("SELECT 1") → table with sql field = "SELECT 1"

test_mysql_query_still_works
    mysql.query("SELECT 1") → raw packet, byte[4]==0x03, NOT a prepared blob
```

---

### 9. `scripts/mysql_prepared.lua` — example workload

```lua
-- scripts/mysql_prepared.lua
--
-- MySQL prepared-statement workload for wrkx (ADR 0005, P6-5).
-- Demonstrates per-request parameterization via mysql.execute().
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/mysql_prepared.lua \
--          mysql://wrkx:secret@localhost/wrkx

local stmt = mysql.prepare("SELECT ?")
local counter = 0

function request()
    counter = counter + 1
    return mysql.execute(stmt, tostring(counter % 1000))
end
```

---

### 10. `tests/e2e/mysql_prepared.sh` — E2E test

Requires `MYSQL_URL` env var; skips cleanly if unset.  Requires MySQL 8.0+.

```
PASS  build with prepared-statement support (make EXTENSIONS="redis memcached postgres mysql")
PASS  basic prepared query: -t1 -c1 -d2s -R10 mysql_prepared.lua exits 0
PASS  parameterized throughput: -t1 -c4 -d5s -R100 > 0 requests, 0 errors
PASS  second request on same connection reuses stmt_id (no re-prepare visible in
      MySQL performance_schema.prepared_statements_instances; or verified via
      row count: 2 connections × N requests = N prepare events, not 2N)
PASS  SQL change mid-workload: second mysql.execute("SELECT ?+1", "5") on a
      connection that previously prepared "SELECT ?" triggers re-prepare
      (closes old stmt, prepares new one)
PASS  wrong-type parameter (non-string number): mysql.execute("SELECT ?", 99)
      succeeds (Lua number coerced to string)
PASS  QuickJS parity: scripts/mysql_prepared.js (mirror of .lua) produces
      matching non-zero request count
PASS  frozen-file diff clean (no changes to core engine, redis, postgres)
```

`scripts/mysql_prepared.js` — QuickJS equivalent of `mysql_prepared.lua`.

---

## Guards

1. `make test` — new `test_mysql_stmt_codec` and all existing tests pass; no
   regressions in `test_mysql_codec`, `test_mysql_lua`, `test_pg_codec`, etc.
2. `make EXTENSIONS="redis memcached postgres mysql" test-asan` — clean under
   ASAN; `test_mysql_lua` excluded per LuaJIT policy; no leaks in codec or
   state machine
3. `MYSQL_URL=... make EXTENSIONS="redis memcached postgres mysql" test` —
   `tests/e2e/mysql_prepared.sh` passes; skips cleanly without `MYSQL_URL`
4. Frozen-file diff is empty: `src/orchestrator.*`, `src/ae.*`, `src/rate.*`,
   `src/net.*`, `src/transport.*`, `include/wrkx_extension.h`,
   `include/wrkx_transport.h`, `extensions/redis/`, `extensions/postgres/`
5. `scripts/adr-compliance.sh` passes

## Core engine touch

Zero.  All new code is within `extensions/mysql/`, `tests/unit/`,
`tests/e2e/`, and `scripts/`.  `wrkx_extension.h` must not change.

## Out of scope for P6-5

- Full binary type encoding (int, float, datetime) — parameters are text-mode
  (MYSQL_TYPE_VAR_STRING); the server transparently converts string→typed value
- Multiple cached prepared statements per connection — one slot is enough for
  typical load-test workloads where all requests use the same query shape
- COM_STMT_RESET / COM_STMT_SEND_LONG_DATA / cursor-based fetch
- mysql+tls:// — defer; TLS negotiation in MySQL requires post-greeting plaintext
  upgrade, a separate task
- caching_sha2_password full exchange (RSA path) — defer
- Binary result-set field decoding — rows counted and bytes accumulated only,
  same as P6-4 text rows
- Full short-write buffering for `mysql_write()` — the existing P6-4 path does
  not check `written == len` on `transport_write()`; that gap is inherited and
  out of scope here.  The new `send_execute` path inside `readable()` does
  check for short writes and returns `PROTO_ERROR` (a short write from
  `readable()` cannot be retried, so treating it as fatal is correct).
  COM_STMT_CLOSE in `mysql_close()` teardown is best-effort (short writes
  ignored) because the connection is being destroyed anyway.  The
  close-before-reprepare path in `mysql_write()` is strict: it checks both
  `TRANSPORT_OK` and `cw == cn` and returns -1 on failure, because a failed
  close there leaves the stream corrupted for the subsequent COM_STMT_PREPARE.
