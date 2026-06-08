/*
 * pg_quickjs_helpers.c — PostgreSQL QuickJS glue module.
 *
 * Mirrors pg_lua_helpers.c under the "postgres@quickjs" namespace.
 * Follows the redis_quickjs_helpers.c pattern exactly.
 *
 * ADR 0005, Phase 6 (P6-3).
 */

#ifdef WRKX_HAVE_QUICKJS

#include "pg_quickjs_helpers.h"
#include "pg_message.h"
#include "pg_result.h"

#include <string.h>

#define PG_MAX_PARAMS 64

/* -------------------------------------------------------------------------
 * pg.query(sql) -> wire bytes
 * ---------------------------------------------------------------------- */

static int qjs_pg_query(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "pg.query: expected one string argument");
        return -1;
    }
    size_t sql_len;
    const char *sql = JS_ToCStringLen(h->ctx, &sql_len, h->argv[0]);
    if (!sql) { h->ret = JS_EXCEPTION; return -1; }

    char buf[65536];
    int n = pg_encode_query(buf, sizeof(buf), sql);
    JS_FreeCString(h->ctx, sql);

    if (n <= 0) {
        h->ret = JS_ThrowInternalError(h->ctx, "pg.query: SQL too large");
        return -1;
    }
    h->ret = JS_NewStringLen(h->ctx, buf, (size_t)n);
    return 0;
}

/* -------------------------------------------------------------------------
 * pg.prepare(sql) -> {sql: sql}
 * ---------------------------------------------------------------------- */

static int qjs_pg_prepare(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "pg.prepare: expected one SQL string");
        return -1;
    }
    JSValue obj = JS_NewObject(h->ctx);
    JS_SetPropertyStr(h->ctx, obj, "sql", JS_DupValue(h->ctx, h->argv[0]));
    h->ret = obj;
    return 0;
}

/* -------------------------------------------------------------------------
 * pg.execute(stmt, param1, ...) -> wire bytes
 * ---------------------------------------------------------------------- */

static int qjs_pg_execute(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "pg.execute: expected SQL or pg.prepare() handle");
        return -1;
    }

    const char *sql = NULL;
    size_t      sql_freed = 0;   /* 1 if we called JS_FreeCString */

    if (JS_IsString(h->argv[0])) {
        size_t dummy;
        sql = JS_ToCStringLen(h->ctx, &dummy, h->argv[0]);
        sql_freed = 1;
    } else if (JS_IsObject(h->argv[0])) {
        JSValue jsql = JS_GetPropertyStr(h->ctx, h->argv[0], "sql");
        if (!JS_IsString(jsql)) {
            JS_FreeValue(h->ctx, jsql);
            h->ret = JS_ThrowTypeError(h->ctx,
                         "pg.execute: invalid pg.prepare() handle");
            return -1;
        }
        size_t dummy;
        sql = JS_ToCStringLen(h->ctx, &dummy, jsql);
        JS_FreeValue(h->ctx, jsql);
        sql_freed = 1;
    } else {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "pg.execute: first arg must be string or handle");
        return -1;
    }
    if (!sql) { h->ret = JS_EXCEPTION; return -1; }

    int n_params = h->argc - 1;
    if (n_params > PG_MAX_PARAMS) {
        JS_FreeCString(h->ctx, sql);
        h->ret = JS_ThrowRangeError(h->ctx,
                     "pg.execute: too many parameters (max %d)", PG_MAX_PARAMS);
        return -1;
    }

    const char *params[PG_MAX_PARAMS];
    size_t      param_lens[PG_MAX_PARAMS];
    int         nconv = 0;

    for (int i = 0; i < n_params; i++) {
        JSValueConst arg = h->argv[i + 1];
        if (JS_IsNull(arg) || JS_IsUndefined(arg)) {
            params[i]     = NULL;
            param_lens[i] = 0;
        } else {
            params[i] = JS_ToCStringLen(h->ctx, &param_lens[i], arg);
            if (!params[i]) {
                for (int j = 0; j < nconv; j++)
                    JS_FreeCString(h->ctx, params[j]);
                JS_FreeCString(h->ctx, sql);
                h->ret = JS_EXCEPTION;
                return -1;
            }
            nconv++;
        }
    }

    char   buf[131072];
    size_t pos = 0;
    int    rc;

    rc = pg_encode_parse(buf + pos, sizeof(buf) - pos, "", sql);
    if (rc <= 0) goto encode_err;
    pos += (size_t)rc;

    rc = pg_encode_bind(buf + pos, sizeof(buf) - pos, "", "",
                        params, param_lens, (int16_t)n_params);
    if (rc <= 0) goto encode_err;
    pos += (size_t)rc;

    rc = pg_encode_describe(buf + pos, sizeof(buf) - pos, 'P', "");
    if (rc <= 0) goto encode_err;
    pos += (size_t)rc;

    rc = pg_encode_execute(buf + pos, sizeof(buf) - pos, "", 0);
    if (rc <= 0) goto encode_err;
    pos += (size_t)rc;

    rc = pg_encode_sync(buf + pos, sizeof(buf) - pos);
    if (rc <= 0) goto encode_err;
    pos += (size_t)rc;

    for (int i = 0; i < nconv; i++) JS_FreeCString(h->ctx, params[i]);
    JS_FreeCString(h->ctx, sql);
    h->ret = JS_NewStringLen(h->ctx, buf, pos);
    return 0;

encode_err:
    for (int i = 0; i < nconv; i++) JS_FreeCString(h->ctx, params[i]);
    JS_FreeCString(h->ctx, sql);
    h->ret = JS_ThrowInternalError(h->ctx, "pg.execute: encoding failed");
    return -1;
}

/* -------------------------------------------------------------------------
 * pg.result() -> object or null
 * ---------------------------------------------------------------------- */

static int qjs_pg_result(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (!tls_result.valid) {
        h->ret = JS_NULL;
        return 0;
    }

    JSValue obj = JS_NewObject(h->ctx);

    JS_SetPropertyStr(h->ctx, obj, "ncols",
                      JS_NewInt32(h->ctx, tls_result.ncols));
    JS_SetPropertyStr(h->ctx, obj, "nrows",
                      JS_NewInt32(h->ctx, tls_result.nrows));
    JS_SetPropertyStr(h->ctx, obj, "cmd_tag",
                      JS_NewString(h->ctx, tls_result.cmd_tag));

    char status_str[2] = { (char)tls_result.pg_status, '\0' };
    JS_SetPropertyStr(h->ctx, obj, "status",
                      JS_NewString(h->ctx, status_str));

    JSValue cols_arr = JS_NewArray(h->ctx);
    for (int c = 0; c < tls_result.ncols; c++) {
        JS_SetPropertyUint32(h->ctx, cols_arr, (uint32_t)c,
                             JS_NewString(h->ctx, tls_result.cols[c].name));
    }
    JS_SetPropertyStr(h->ctx, obj, "cols", cols_arr);

    JSValue rows_arr = JS_NewArray(h->ctx);
    for (int r = 0; r < tls_result.nrows; r++) {
        JSValue row_arr = JS_NewArray(h->ctx);
        for (int c = 0; c < tls_result.ncols; c++) {
            const char *v = tls_result.fields[r][c].value;
            JSValue fval = (v == NULL)
                         ? JS_NULL
                         : JS_NewStringLen(h->ctx, v,
                                           tls_result.fields[r][c].len);
            JS_SetPropertyUint32(h->ctx, row_arr, (uint32_t)c, fval);
        }
        JS_SetPropertyUint32(h->ctx, rows_arr, (uint32_t)r, row_arr);
    }
    JS_SetPropertyStr(h->ctx, obj, "rows", rows_arr);

    h->ret = obj;
    return 0;
}

/* -------------------------------------------------------------------------
 * Transaction helpers
 * ---------------------------------------------------------------------- */

static int qjs_pg_begin(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    char buf[16];
    int n = pg_encode_query(buf, sizeof(buf), "BEGIN");
    if (n <= 0) { h->ret = JS_EXCEPTION; return -1; }
    h->ret = JS_NewStringLen(h->ctx, buf, (size_t)n);
    return 0;
}

static int qjs_pg_commit(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    char buf[16];
    int n = pg_encode_query(buf, sizeof(buf), "COMMIT");
    if (n <= 0) { h->ret = JS_EXCEPTION; return -1; }
    h->ret = JS_NewStringLen(h->ctx, buf, (size_t)n);
    return 0;
}

static int qjs_pg_rollback(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    char buf[16];
    int n = pg_encode_query(buf, sizeof(buf), "ROLLBACK");
    if (n <= 0) { h->ret = JS_EXCEPTION; return -1; }
    h->ret = JS_NewStringLen(h->ctx, buf, (size_t)n);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public helper table
 * ---------------------------------------------------------------------- */

const script_helper postgres_quickjs_helpers[] = {
    { "query",    qjs_pg_query    },
    { "prepare",  qjs_pg_prepare  },
    { "execute",  qjs_pg_execute  },
    { "result",   qjs_pg_result   },
    { "begin",    qjs_pg_begin    },
    { "commit",   qjs_pg_commit   },
    { "rollback", qjs_pg_rollback },
};

const size_t postgres_quickjs_helpers_count =
    sizeof(postgres_quickjs_helpers) / sizeof(postgres_quickjs_helpers[0]);

#endif /* WRKX_HAVE_QUICKJS */
