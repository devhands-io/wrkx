title: postgres extension — P6-2 extended query, parameter binding, Gate E
status: completed
adr: 0005
adr-step: P6-2
depends: t082

## Why / Goal

P6-2 extends the PostgreSQL extension with the extended query protocol (Parse, Bind,
Describe, Execute, Sync), parameter binding, and result metadata.  It closes Gate E:
if multi-step stateful protocol state (client-side parse plan, parameter binding,
result metadata) is handled entirely inside the vtable with zero leakage into the
orchestrator or scheduler, the claim of ADR 0005 Phase 6 is confirmed.

The key design choice is **anonymous prepared statements** (statement name = `""`).
PostgreSQL replaces the unnamed statement slot when a new Parse(`""`, …) arrives, so
sending Parse + Bind + Describe('P') + Execute + Sync on every request means each
request starts with a fresh plan and the previous unnamed statement is implicitly
superseded.  The client receives ParseComplete + BindComplete + RowDescription (or
NoData) + DataRow* + CommandComplete + ReadyForQuery.  This maps exactly to the
existing write/readable cycle with no per-connection parse-lifecycle tracking, and it
still proves the architectural claim: multi-step protocol state lives in the vtable,
not the orchestrator.

Named prepared statements with explicit `Close`/reuse across requests — where the
client must track which statement names have been parsed on each connection — are
deferred to P6-3.

---

## Deliverables

### 1. `extensions/postgres/pg_message.c` / `pg_message.h` — codec additions

**New encode functions** (added to existing `pg_message.c`):

| Function | Wire format |
|---|---|
| `pg_encode_parse(buf, cap, name, sql)` | `'P' + int32_len + name\0 + sql\0 + int16(0)` (zero param-type OIDs) |
| `pg_encode_bind(buf, cap, portal, stmt, params, lens, n)` | `'B' + int32_len + portal\0 + stmt\0 + int16(0) + int16(n) + [int32(len)+bytes …] + int16(0)` |
| `pg_encode_describe(buf, cap, type, name)` | `'D' + int32_len + type('S' or 'P') + name\0` |
| `pg_encode_execute(buf, cap, portal, max_rows)` | `'E' + int32_len + portal\0 + int32(max_rows)` |
| `pg_encode_sync(buf, cap)` | `'S' + int32(4)` |
| `pg_encode_close_stmt(buf, cap, name)` | `'C' + int32_len + 'S' + name\0` |

All return bytes written (> 0) or ≤ 0 on buffer-too-small.

`pg_encode_bind` NULL-param handling: if `params[i]` is NULL, encode `int32(-1)` for
that slot (PostgreSQL NULL representation).

**New parse types** (added to `pg_msg_type` enum and handled in `pg_parse_message`):

```c
PG_MSG_PARSE_COMPLETE,        /* '1' — no payload beyond the 4-byte length  */
PG_MSG_BIND_COMPLETE,         /* '2' — no payload                            */
PG_MSG_CLOSE_COMPLETE,        /* '3' — no payload                            */
PG_MSG_PARAMETER_DESCRIPTION, /* 't' — int16 n_params + [int32 type_oid …]  */
PG_MSG_NO_DATA,               /* 'n' — no payload; non-SELECT statements     */
```

`PG_MSG_PARAMETER_DESCRIPTION`: store only `int16 n_params`; skip the OID array.

**`pg_parsed_msg.row_description` extension** — P6-2 adds column names to the union
member so that `readable()` can store result metadata in `pg_state` without leaking
state outside the vtable:

```c
#define PG_MAX_COLS 64

/* in pg_parsed_msg union: */
struct {
    int16_t ncols;                               /* clamped to PG_MAX_COLS         */
    struct { char name[64]; } cols[PG_MAX_COLS]; /* valid indices: 0 .. ncols-1    */
} row_description;
```

`pg_parse_message` sets `ncols = MIN(server_ncols, PG_MAX_COLS)` and fills only
`cols[0..ncols-1]`; columns beyond `PG_MAX_COLS` are skipped.  The true server count
is not stored — callers must treat `ncols` as both the clamped stored count and the
safe iteration bound.

`readable()` likewise assigns `s->n_cols = MIN(msg.row_description.ncols, PG_MAX_COLS)`
(which is already clamped, so this is a copy, but the explicit MIN guards against
future changes to `pg_parsed_msg`).

The `pg_parsed_msg` struct grows substantially but remains stack-allocated in callers
(it is a local in `readable()`).  If this proves too large for stack use, allocate it
on the heap — decide at implementation time.

---

### 2. `extensions/postgres/postgres.c` — `pg_state` and `readable()` update

**`pg_state` additions** — stores result metadata in `proto_state` without leaking
outside the vtable (this is the architectural proof Gate E requires):

```c
#define PG_MAX_COLS 64

typedef struct {
    char name[64];
} pg_col_info;

typedef struct pg_state {
    transport  xport;
    char       rbuf[PG_RECVBUF];
    size_t     rbuf_len;
    pg_phase   phase;
    bool       done;
    bool       error;
    size_t     bytes;
    int32_t    row_count;
    /* result metadata — populated by RowDescription during readable() */
    pg_col_info columns[PG_MAX_COLS];
    int16_t     n_cols;
} pg_state;
```

**`readable()` update** — the `pg_phase` enum is **unchanged**.  Extended query
responses flow through the same `READY_FOR_QUERY`-terminating loop.  Add handling for
the six new message types:

```
- PARSE_COMPLETE        → consume; continue
- BIND_COMPLETE         → consume; continue
- NO_DATA               → s->n_cols = 0; consume; continue  (non-SELECT: no result columns)
- PARAMETER_DESCRIPTION → consume; continue
- CLOSE_COMPLETE        → consume; continue
- ROW_DESCRIPTION       → s->n_cols = MIN(msg.row_description.ncols, PG_MAX_COLS);
                           memcpy(s->columns, msg.row_description.cols,
                                  s->n_cols * sizeof(s->columns[0]));
                           consume; continue
                           (P6-1 only consumed without storing — update that path too)
```

The `ROW_DESCRIPTION` case now stores column names in `pg_state.columns` for both
the simple query path (P6-1) and the extended query path (P6-2).  P6-3 will expose
this via a `pg.columns()` Lua helper; P6-2 just proves the state lives correctly in
`proto_state`.

**`write()` update** — reset `s->n_cols = 0` alongside `s->done`, `s->error`,
`s->bytes`, and `s->row_count` when a new request arrives.  This prevents a prior
SELECT's column names from being visible after a subsequent non-SELECT (which emits
`NoData` and never sets `n_cols`).

---

### 3. `extensions/postgres/pg_lua_helpers.c` — new helpers

**`pg.prepare(sql)`** — validates the SQL string and returns a Lua table
`{sql = sql}` as an opaque prepared-statement handle.  No wire bytes are generated.
This is a client-side-only step; its purpose is ergonomics (name the query once,
reuse the handle) and future extension (P6-3 can add server-side parse here without
changing the call site).

```c
static int lua_pg_prepare(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "pg.prepare: expected one SQL string");
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "sql");
    return 1;
}
```

**`pg.execute(stmt, param1, ...)`** — accepts either a raw SQL string or a
`pg.prepare()` table as the first argument; subsequent arguments are query
parameters (strings, numbers, or nil for SQL NULL).  Returns
Parse("") + Bind("") + Describe('P', "") + Execute("") + Sync wire bytes as a Lua
string.  The Describe-portal step causes the server to emit `RowDescription` (or
`NoData` for non-SELECT), which is what exercises the `pg_state.columns` metadata
path added in Deliverable 2.

```c
#define PG_MAX_PARAMS 64

static int lua_pg_execute(void *engine_ctx) {
    lua_State *L = (lua_State *)engine_ctx;
    int nargs = lua_gettop(L);
    if (nargs < 1)
        return luaL_error(L, "pg.execute: expected sql or pg.prepare() handle");

    const char *sql = NULL;
    if (lua_type(L, 1) == LUA_TSTRING) {
        sql = lua_tostring(L, 1);
    } else if (lua_type(L, 1) == LUA_TTABLE) {
        lua_getfield(L, 1, "sql");
        if (lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "pg.execute: invalid pg.prepare() handle");
        sql = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else {
        return luaL_error(L, "pg.execute: first arg must be SQL string or pg.prepare() handle");
    }

    int n_params = nargs - 1;
    if (n_params > PG_MAX_PARAMS)
        return luaL_error(L, "pg.execute: too many parameters (max %d)", PG_MAX_PARAMS);

    const char *params[PG_MAX_PARAMS];
    size_t      param_lens[PG_MAX_PARAMS];

    for (int i = 0; i < n_params; i++) {
        int idx = i + 2;
        if (lua_isnil(L, idx)) {
            params[i] = NULL;
            param_lens[i] = 0;
        } else if (lua_type(L, idx) == LUA_TSTRING || lua_type(L, idx) == LUA_TNUMBER) {
            params[i] = lua_tolstring(L, idx, &param_lens[i]);
        } else {
            return luaL_error(L, "pg.execute: param %d must be string, number, or nil", i + 1);
        }
    }

    char   buf[131072];   /* 128 KiB: ample for any realistic query + params */
    size_t pos = 0;
    int    n;

    n = pg_encode_parse(buf + pos, sizeof(buf) - pos, "", sql);
    if (n <= 0) return luaL_error(L, "pg.execute: SQL too large");
    pos += (size_t)n;

    n = pg_encode_bind(buf + pos, sizeof(buf) - pos, "", "",
                       params, param_lens, (int16_t)n_params);
    if (n <= 0) return luaL_error(L, "pg.execute: parameter encoding failed");
    pos += (size_t)n;

    n = pg_encode_describe(buf + pos, sizeof(buf) - pos, 'P', "");
    if (n <= 0) return luaL_error(L, "pg.execute: buffer overflow");
    pos += (size_t)n;

    n = pg_encode_execute(buf + pos, sizeof(buf) - pos, "", 0);
    if (n <= 0) return luaL_error(L, "pg.execute: buffer overflow");
    pos += (size_t)n;

    n = pg_encode_sync(buf + pos, sizeof(buf) - pos);
    if (n <= 0) return luaL_error(L, "pg.execute: buffer overflow");
    pos += (size_t)n;

    lua_pushlstring(L, buf, pos);
    return 1;
}
```

Register both helpers in the `"postgres@lua"` namespace (alongside the existing
`pg.query`):

```c
const script_helper postgres_lua_helpers[] = {
    { "query",   lua_pg_query   },
    { "prepare", lua_pg_prepare },
    { "execute", lua_pg_execute },
};
```

`init.c` and `Makefile.ext` do not change (no new files).

---

### 4. `tests/unit/test_pg_codec.c` — codec test additions

Extend the existing test file from P6-1 with tests for the new encode/parse paths:

```
test_encode_parse_anonymous
    pg_encode_parse(buf, cap, "", "SELECT $1::int") →
    tag='P', length correct, name="\0", sql correct, int16(0) param OIDs

test_encode_parse_named
    pg_encode_parse(buf, cap, "mystmt", "SELECT 1") →
    name="mystmt\0", sql correct

test_encode_bind_no_params
    pg_encode_bind(buf, cap, "", "", NULL, NULL, 0) →
    tag='B', portals/stmt both "\0", int16(0) fmt codes,
    int16(0) params, int16(0) result fmt codes

test_encode_bind_two_text_params
    params={"hello", "42"} → correct int32 lengths + bytes, no null terminator

test_encode_bind_null_param
    params={NULL, "x"} → first slot is int32(-1), second is normal

test_encode_execute_anonymous
    pg_encode_execute(buf, cap, "", 0) →
    tag='E', portal="\0", max_rows=0

test_encode_execute_row_limit
    max_rows=10 → int32(10) in correct position

test_encode_sync
    pg_encode_sync(buf, cap) → tag='S', int32(4), total 5 bytes

test_encode_describe_statement
    pg_encode_describe(buf, cap, 'S', "mystmt") →
    tag='D', 'S', "mystmt\0", correct length

test_encode_describe_portal
    pg_encode_describe(buf, cap, 'P', "") →
    tag='D', 'P', "\0"

test_encode_close_stmt
    pg_encode_close_stmt(buf, cap, "mystmt") →
    tag='C', 'S', "mystmt\0"

test_parse_row_description_column_names
    two-column RowDescription with names "id" and "val" →
    PG_MSG_ROW_DESCRIPTION, ncols=2,
    cols[0].name="id", cols[1].name="val"

test_parse_row_description_name_truncation
    column name longer than 63 bytes → name truncated to 63 chars + '\0', no overflow

test_parse_row_description_clamp_at_max_cols
    synthetic RowDescription with PG_MAX_COLS + 1 (65) columns →
    PG_MSG_ROW_DESCRIPTION, ncols == PG_MAX_COLS (64), not 65;
    cols[63].name accessible without overflow;
    bytes consumed equals the full message length (all 65 column descriptors
    walked and skipped past the 64th, no short-read)

test_parse_parse_complete
    "\x31\x00\x00\x00\x04" → PG_MSG_PARSE_COMPLETE, 5 bytes consumed

test_parse_bind_complete
    "\x32\x00\x00\x00\x04" → PG_MSG_BIND_COMPLETE, 5 bytes consumed

test_parse_close_complete
    "\x33\x00\x00\x00\x04" → PG_MSG_CLOSE_COMPLETE, 5 bytes consumed

test_parse_no_data
    "\x6e\x00\x00\x00\x04" → PG_MSG_NO_DATA, 5 bytes consumed

test_parse_parameter_description_zero
    "\x74\x00\x00\x00\x06\x00\x00" → PG_MSG_PARAMETER_DESCRIPTION, n_params=0

test_encode_parse_sync_sequence_lengths
    encode Parse("", "SELECT 1") + Sync into one buffer (frontend messages only;
    pg_parse_message is not called — it is a backend-response parser and must not
    be fed frontend frames); walk the buffer by tag + int32 length field to verify:
    buf[0] == 'P' (Parse tag),
    buf[offset] == 'S' (Sync tag) where offset = 1 + ntohl(parse_len),
    total bytes == pg_encode_parse() + pg_encode_sync() return values,
    no bytes overlap
```

---

### 5. `tests/unit/test_pg_lua.c` — Lua helper unit tests

New test binary (mirrors `test_mc_lua.c`).  Tests the Lua helpers without a live
PostgreSQL server.

```
test_pg_prepare_returns_table
    pg.prepare("SELECT 1") → Lua table with .sql == "SELECT 1"

test_pg_prepare_wrong_type_errors
    pg.prepare(42) → Lua error, no crash

test_pg_execute_with_string_sql
    pg.execute("SELECT $1::int", "42") → Lua string starting with 'P' (Parse tag)

test_pg_execute_with_prepare_handle
    local s = pg.prepare("SELECT $1"); pg.execute(s, "7") →
    same wire bytes as pg.execute("SELECT $1", "7")

test_pg_execute_no_params
    pg.execute("SELECT 1") → wire buffer: Parse + Bind(0 params) + Describe('P') + Execute + Sync

test_pg_execute_describe_position
    pg.execute("SELECT $1", "x") → parse the wire buffer sequentially;
    verify tag order is 'P', 'B', 'D', 'E', 'S' (Describe immediately after Bind,
    before Execute); verify Describe type byte is 'P' and portal name is "\0"

test_pg_execute_null_param
    pg.execute("INSERT INTO t VALUES($1)", nil) →
    Bind contains int32(-1) in first param slot

test_pg_execute_number_param
    pg.execute("SELECT $1", 3.14) → param encoded as string "3.14"

test_pg_execute_too_many_params_errors
    pass PG_MAX_PARAMS+1 params → Lua error

test_pg_execute_invalid_handle_errors
    pg.execute({}) → Lua error ("invalid pg.prepare() handle")

test_pg_query_still_works
    pg.query("SELECT 1") → wire buffer starts with 'Q' tag
    (regression: P6-2 changes must not break the P6-1 helper)
```

**Makefile additions** (following `LUA_MC_ENGINE_DEPS` pattern):

```makefile
TEST_PG_LUA_SRC      := tests/unit/test_pg_lua.c
TEST_PG_LUA_BIN      := obj/test_pg_lua
LUA_PG_ENGINE_DEPS   := $(LUA_ENGINE_DEPS) \
                        extensions/postgres/pg_message.c \
                        extensions/postgres/pg_lua_helpers.c \
                        extensions/postgres/postgres.c \
                        src/transport.c

$(TEST_PG_LUA_BIN): $(TEST_PG_LUA_SRC) $(UNITY_SRC) $(LUA_PG_ENGINE_DEPS) | $(ODIR)
	$(CC) $(CFLAGS) $(UNITY_INC) -Iextensions/postgres -Isrc \
	      -o $@ $(TEST_PG_LUA_SRC) $(UNITY_SRC) $(LUA_PG_ENGINE_DEPS) \
	      $(LDIR)/libluajit.a $(LIBS)

# Add TEST_PG_LUA_BIN to test-unit prereqs and its run to the test-unit recipe.
# test-unit already excludes *_lua binaries from test-asan (see LuaJIT ASAN note
# in existing Makefile comments); same applies here.
```

---

### 6. `scripts/postgres_prepared.lua` — example prepared-statement workload

```lua
-- scripts/postgres_prepared.lua
--
-- PostgreSQL prepared-statement workload for wrkx (ADR 0005, P6-2).
-- Uses the extended query protocol (Parse + Bind + Describe('P') + Execute + Sync per request).
-- Use with:
--   ./wrkx -t4 -c100 -d10s -R500 -s scripts/postgres_prepared.lua \
--          postgres://wrkx:secret@localhost/wrkx

local counter = 0
local stmt = pg.prepare("SELECT $1::int")

function request()
    counter = counter + 1
    return pg.execute(stmt, tostring(counter % 1000))
end
```

---

### 7. `tests/e2e/postgres_prepared.sh` — E2E test + Gate E

Requires `POSTGRES_URL` env var (skip cleanly if unset).

```
PASS  extended query smoke test: -t1 -c1 -d2s -R10 pg.execute() exits 0, > 0 requests
PASS  multi-param execute: pg.execute("SELECT $1::int + $2::int", "3", "4") — 0 errors
PASS  NULL param: pg.execute("SELECT $1::text", nil) — 0 errors, not a crash
PASS  syntax error via execute: pg.execute("NOT SQL", ...) — non-zero error count, no crash
PASS  pg.query() still works alongside pg.execute() in same script — regression check
PASS  frozen core unchanged since $BASELINE (p6-baseline) — same frozen paths as gate_d.sh
```

**Gate E check** (inline in the script, reported as final PASS/FAIL):

```bash
BASELINE="${GATE_E_BASELINE:-p6-baseline}"

FROZEN="src/orchestrator.c src/orchestrator.h \
        src/ae.c src/ae.h src/ae_epoll.c src/ae_kqueue.c \
        src/ae_select.c src/ae_evport.c \
        src/rate.c src/rate.h \
        src/net.c src/net.h \
        src/transport.c src/transport.h \
        include/wrkx_extension.h include/wrkx_transport.h \
        extensions/redis/redis.c extensions/redis/redis.h"

# diff against the fixed pre-P6 baseline tag, not HEAD
check "frozen core+protocol unchanged since $BASELINE" \
    "test -z \"\$(git diff --name-only $BASELINE HEAD -- $FROZEN)\""
```

**`p6-baseline` tagging:** the tag must be created as the first step of t082 (P6-1),
before any `extensions/postgres/` code is committed, and should already exist by the
time P6-2 is implemented.

**At the start of t082:** `git tag p6-baseline` — this is a prerequisite for t083.

`postgres_prepared.sh` must fail with a clear error if the tag is missing:

```bash
if ! git rev-parse --verify "$BASELINE" >/dev/null 2>&1; then
    echo "FAIL  $BASELINE tag not found"
    echo "      Recovery: find the last commit before extensions/postgres/ was added,"
    echo "      then: git tag p6-baseline <that-commit>"
    exit 1
fi
```

This mirrors the `p5-baseline` tag used by `gate_d.sh`.

Gate E is confirmed if all six checks pass.  Report:

```
Gate E: N passed, 0 failed
```

(or fail with the specific check name if any FAIL).

---

## Guards

1. `make test` — `test_pg_codec` passes with new codec tests added; `test_pg_lua`
   passes (links `pg_message.c` + `pg_lua_helpers.c`, no live PostgreSQL); all
   existing tests still pass
2. `make EXTENSIONS="redis memcached postgres" test-asan` — clean; no memory leaks
   in new codec paths or Lua helpers (LuaJIT excluded from ASAN as per existing
   Makefile policy)
3. `POSTGRES_URL=postgres://user:pw@host/db make EXTENSIONS="redis memcached postgres" test`
   — `tests/e2e/postgres_prepared.sh` passes including Gate E confirmation
4. `pg.query()` regression: `tests/e2e/postgres_basic.sh` (P6-1 E2E) still passes
   unchanged — adding `pg.execute()` must not break the simple query path
5. Frozen-file diff is empty: `src/orchestrator.*`, `src/ae.*`, `src/rate.*`,
   `src/net.*`, `src/transport.*`, `include/wrkx_extension.h`,
   `include/wrkx_transport.h`, `extensions/redis/`

## Core engine touch

Zero.  All changes are within `extensions/postgres/` and `tests/unit/`, `tests/e2e/`,
`scripts/`.

`wrkx_extension.h` must **not** change.

## Out of scope for P6-2

- **Named prepared statements with per-connection lifecycle (P6-3)** — requires
  tracking which statement names have been parsed per connection and issuing `Close`
  when a connection is recycled.  P6-2 uses anonymous statements only.
- **`pg.columns()` Lua helper (P6-3)** — column names are stored in `pg_state` by
  P6-2 but not yet exposed to scripts; the Lua bridge belongs in P6-3 alongside the
  full result-set decoding API.
- **Result-set field decoding / typed Lua result objects (P6-3)** — DataRow field
  values are counted and byte-accumulated in P6-2 but not decoded.
- SCRAM-SHA-256 auth (P6-3)
- TLS / SSLRequest prelude (P6-3)
- QuickJS helpers — `pg@quickjs` namespace is a follow-on after Gate E is confirmed
