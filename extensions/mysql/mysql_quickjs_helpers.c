/*
 * mysql_quickjs_helpers.c — MySQL QuickJS glue module.
 *
 * Mirrors mysql_lua_helpers.c under the "mysql@quickjs" namespace.
 * Follows the pg_quickjs_helpers.c pattern exactly.
 *
 * ADR 0005, Phase 6 (P6-5).
 */

#ifdef WRKX_HAVE_QUICKJS

#include "mysql_quickjs_helpers.h"
#include "mysql_packet.h"

#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * mysql.query(sql) -> wire bytes
 * ---------------------------------------------------------------------- */

static int qjs_mysql_query(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "mysql.query: expected one string argument");
        return -1;
    }
    size_t sql_len;
    const char *sql = JS_ToCStringLen(h->ctx, &sql_len, h->argv[0]);
    if (!sql) { h->ret = JS_EXCEPTION; return -1; }

    uint8_t buf[65540];
    int n = mysql_encode_com_query(buf, sizeof(buf), sql, sql_len);
    JS_FreeCString(h->ctx, sql);

    if (n <= 0) {
        h->ret = JS_ThrowInternalError(h->ctx, "mysql.query: SQL too large");
        return -1;
    }
    h->ret = JS_NewStringLen(h->ctx, (const char *)buf, (size_t)n);
    return 0;
}

/* -------------------------------------------------------------------------
 * mysql.prepare(sql) -> {sql: sql}
 * ---------------------------------------------------------------------- */

static int qjs_mysql_prepare(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "mysql.prepare: expected one SQL string");
        return -1;
    }
    JSValue obj = JS_NewObject(h->ctx);
    JS_SetPropertyStr(h->ctx, obj, "sql", JS_DupValue(h->ctx, h->argv[0]));
    h->ret = obj;
    return 0;
}

/* -------------------------------------------------------------------------
 * mysql.execute(handle_or_sql, param1, ...) -> internal blob bytes
 * ---------------------------------------------------------------------- */

static int qjs_mysql_execute(void *engine_ctx) {
    qjs_helper_ctx *h = (qjs_helper_ctx *)engine_ctx;
    if (h->argc < 1) {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "mysql.execute: expected SQL or mysql.prepare() handle");
        return -1;
    }

    const char *sql = NULL;
    size_t      sql_len = 0;
    int         sql_freed = 0;   /* 1 if we called JS_FreeCString on sql */

    if (JS_IsString(h->argv[0])) {
        sql = JS_ToCStringLen(h->ctx, &sql_len, h->argv[0]);
        if (!sql) { h->ret = JS_EXCEPTION; return -1; }
        sql_freed = 1;
    } else if (JS_IsObject(h->argv[0])) {
        JSValue jsql = JS_GetPropertyStr(h->ctx, h->argv[0], "sql");
        if (!JS_IsString(jsql)) {
            JS_FreeValue(h->ctx, jsql);
            h->ret = JS_ThrowTypeError(h->ctx,
                         "mysql.execute: invalid mysql.prepare() handle");
            return -1;
        }
        sql = JS_ToCStringLen(h->ctx, &sql_len, jsql);
        JS_FreeValue(h->ctx, jsql);
        if (!sql) { h->ret = JS_EXCEPTION; return -1; }
        sql_freed = 1;
    } else {
        h->ret = JS_ThrowTypeError(h->ctx,
                     "mysql.execute: first arg must be string or handle");
        return -1;
    }

    /* Enforce cache-key limit before encoding */
    if (sql_len > MYSQL_MAX_PREPARED_SQL) {
        JS_FreeCString(h->ctx, sql);
        h->ret = JS_ThrowTypeError(h->ctx,
                     "mysql.execute: SQL exceeds %d-byte limit",
                     MYSQL_MAX_PREPARED_SQL);
        return -1;
    }

    int n_params = h->argc - 1;
    if (n_params > 127) {
        JS_FreeCString(h->ctx, sql);
        h->ret = JS_ThrowRangeError(h->ctx,
                     "mysql.execute: too many parameters (max 127)");
        return -1;
    }

    const char *params[128];
    size_t      param_lens[128];
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

    uint8_t buf[65540];   /* must match sizeof(s->pending) in mysql.c */
    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          sql, sql_len,
                                          params, param_lens, n_params);

    for (int i = 0; i < nconv; i++) JS_FreeCString(h->ctx, params[i]);
    JS_FreeCString(h->ctx, sql);
    (void)sql_freed;

    if (n <= 0) {
        h->ret = JS_ThrowInternalError(h->ctx,
                     "mysql.execute: encoding failed");
        return -1;
    }
    h->ret = JS_NewStringLen(h->ctx, (const char *)buf, (size_t)n);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public helper table
 * ---------------------------------------------------------------------- */

const script_helper mysql_quickjs_helpers[] = {
    { "query",   qjs_mysql_query   },
    { "prepare", qjs_mysql_prepare },
    { "execute", qjs_mysql_execute },
};

const size_t mysql_quickjs_helpers_count =
    sizeof(mysql_quickjs_helpers) / sizeof(mysql_quickjs_helpers[0]);

#endif /* WRKX_HAVE_QUICKJS */
