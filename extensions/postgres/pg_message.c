/*
 * pg_message.c — PostgreSQL wire-protocol codec.
 *
 * ADR 0005, Phase 6 (P6-1 + P6-2).
 * No networking, no wrkx engine headers.
 */

#include "pg_message.h"

#include <string.h>
#include <stdio.h>
#include <openssl/evp.h>

/* -------------------------------------------------------------------------
 * Big-endian helpers (avoid arpa/inet.h in extension code)
 * ---------------------------------------------------------------------- */

static void put_i32(char *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (char)((u >> 24) & 0xff);
    p[1] = (char)((u >> 16) & 0xff);
    p[2] = (char)((u >>  8) & 0xff);
    p[3] = (char)( u        & 0xff);
}

static void put_i16(char *p, int16_t v) {
    uint16_t u = (uint16_t)v;
    p[0] = (char)((u >> 8) & 0xff);
    p[1] = (char)( u       & 0xff);
}

static int32_t get_i32(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return (int32_t)(((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
                     ((uint32_t)u[2] <<  8) |  (uint32_t)u[3]);
}

static int16_t get_i16(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return (int16_t)(((uint16_t)u[0] << 8) | (uint16_t)u[1]);
}

/* -------------------------------------------------------------------------
 * Frontend message encoders — P6-1
 * ---------------------------------------------------------------------- */

int pg_encode_startup(char *buf, size_t cap,
                      const char *user, const char *db) {
    /* int32 length + int32 protocol + "user\0<user>\0database\0<db>\0\0" */
    size_t user_len = strlen(user);
    size_t db_len   = strlen(db);
    size_t body     = 4                          /* protocol version */
                    + 5 + user_len + 1           /* "user\0" + user + \0 */
                    + 9 + db_len   + 1           /* "database\0" + db + \0 */
                    + 1;                         /* terminating \0 */
    size_t total    = 4 + body;                  /* length field + body */
    if (cap < total) return -1;

    put_i32(buf, (int32_t)total);
    put_i32(buf + 4, 0x00030000);                /* protocol 3.0 */
    char *p = buf + 8;
    memcpy(p, "user", 4); p += 4; *p++ = '\0';
    memcpy(p, user, user_len); p += user_len; *p++ = '\0';
    memcpy(p, "database", 8); p += 8; *p++ = '\0';
    memcpy(p, db, db_len); p += db_len; *p++ = '\0';
    *p++ = '\0';
    return (int)total;
}

int pg_encode_query(char *buf, size_t cap, const char *sql) {
    size_t sql_len = strlen(sql);
    size_t total   = 1 + 4 + sql_len + 1;       /* 'Q' + len + sql + \0 */
    if (cap < total) return -1;
    buf[0] = 'Q';
    put_i32(buf + 1, (int32_t)(4 + sql_len + 1));
    memcpy(buf + 5, sql, sql_len);
    buf[5 + sql_len] = '\0';
    return (int)total;
}

int pg_encode_password(char *buf, size_t cap, const char *password) {
    size_t pass_len = strlen(password);
    size_t total    = 1 + 4 + pass_len + 1;
    if (cap < total) return -1;
    buf[0] = 'p';
    put_i32(buf + 1, (int32_t)(4 + pass_len + 1));
    memcpy(buf + 5, password, pass_len);
    buf[5 + pass_len] = '\0';
    return (int)total;
}

static int hex_md5(const void *input, size_t len, char out32[33]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    unsigned char digest[16];
    unsigned int  dlen = 16;
    int ok = EVP_DigestInit_ex(ctx, EVP_md5(), NULL) &&
             EVP_DigestUpdate(ctx, input, len) &&
             EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;
    for (int i = 0; i < 16; i++)
        snprintf(out32 + i * 2, 3, "%02x", (unsigned)digest[i]);
    out32[32] = '\0';
    return 0;
}

int pg_encode_md5_password(char *buf, size_t cap,
                           const char *password, const char *user,
                           const uint8_t salt[4]) {
    /* inner = MD5(password + user) → 32-char hex */
    size_t pass_len = strlen(password);
    size_t user_len = strlen(user);
    if (pass_len + user_len > 4096) return -1;

    char inner_input[4097];
    memcpy(inner_input, password, pass_len);
    memcpy(inner_input + pass_len, user, user_len);
    char inner_hex[33];
    if (hex_md5(inner_input, pass_len + user_len, inner_hex) != 0) return -1;

    /* outer = MD5(inner_hex + salt) → 32-char hex */
    char outer_input[36];
    memcpy(outer_input, inner_hex, 32);
    memcpy(outer_input + 32, salt, 4);
    char outer_hex[33];
    if (hex_md5(outer_input, 36, outer_hex) != 0) return -1;

    /* "md5" + outer_hex + '\0' = 36 chars */
    char response[36];
    memcpy(response, "md5", 3);
    memcpy(response + 3, outer_hex, 32);
    response[35] = '\0';

    return pg_encode_password(buf, cap, response);
}

/* -------------------------------------------------------------------------
 * Frontend message encoders — P6-2
 * ---------------------------------------------------------------------- */

int pg_encode_parse(char *buf, size_t cap, const char *name, const char *sql) {
    size_t name_len = strlen(name);
    size_t sql_len  = strlen(sql);
    /* 'P' + int32(len) + name\0 + sql\0 + int16(0) */
    size_t total = 1 + 4 + name_len + 1 + sql_len + 1 + 2;
    if (cap < total) return -1;
    buf[0] = 'P';
    put_i32(buf + 1, (int32_t)(4 + name_len + 1 + sql_len + 1 + 2));
    char *p = buf + 5;
    memcpy(p, name, name_len); p += name_len; *p++ = '\0';
    memcpy(p, sql, sql_len);   p += sql_len;  *p++ = '\0';
    put_i16(p, 0);                              /* zero param-type OIDs */
    return (int)total;
}

int pg_encode_bind(char *buf, size_t cap,
                   const char *portal, const char *stmt,
                   const char * const *params,
                   const size_t *param_lens, int16_t n_params) {
    size_t portal_len = strlen(portal);
    size_t stmt_len   = strlen(stmt);
    /* compute body size */
    size_t body = portal_len + 1 + stmt_len + 1
                + 2                              /* int16(0) format codes */
                + 2;                             /* int16(n_params) */
    for (int16_t i = 0; i < n_params; i++)
        body += 4 + (params[i] ? param_lens[i] : 0);
    body += 2;                                   /* int16(0) result fmt codes */

    size_t total = 1 + 4 + body;
    if (cap < total) return -1;

    buf[0] = 'B';
    put_i32(buf + 1, (int32_t)(4 + body));
    char *p = buf + 5;

    memcpy(p, portal, portal_len); p += portal_len; *p++ = '\0';
    memcpy(p, stmt,   stmt_len);   p += stmt_len;   *p++ = '\0';
    put_i16(p, 0); p += 2;                      /* 0 format codes (all text) */
    put_i16(p, n_params); p += 2;
    for (int16_t i = 0; i < n_params; i++) {
        if (params[i] == NULL) {
            put_i32(p, -1); p += 4;             /* SQL NULL */
        } else {
            put_i32(p, (int32_t)param_lens[i]); p += 4;
            memcpy(p, params[i], param_lens[i]); p += param_lens[i];
        }
    }
    put_i16(p, 0);                              /* 0 result format codes */
    return (int)total;
}

int pg_encode_describe(char *buf, size_t cap, char type, const char *name) {
    size_t name_len = strlen(name);
    size_t total = 1 + 4 + 1 + name_len + 1;
    if (cap < total) return -1;
    buf[0] = 'D';
    put_i32(buf + 1, (int32_t)(4 + 1 + name_len + 1));
    buf[5] = type;
    memcpy(buf + 6, name, name_len);
    buf[6 + name_len] = '\0';
    return (int)total;
}

int pg_encode_execute(char *buf, size_t cap,
                      const char *portal, int32_t max_rows) {
    size_t portal_len = strlen(portal);
    size_t total = 1 + 4 + portal_len + 1 + 4;
    if (cap < total) return -1;
    buf[0] = 'E';
    put_i32(buf + 1, (int32_t)(4 + portal_len + 1 + 4));
    memcpy(buf + 5, portal, portal_len);
    buf[5 + portal_len] = '\0';
    put_i32(buf + 6 + portal_len, max_rows);
    return (int)total;
}

int pg_encode_sync(char *buf, size_t cap) {
    if (cap < 5) return -1;
    buf[0] = 'S';
    put_i32(buf + 1, 4);
    return 5;
}

int pg_encode_close_stmt(char *buf, size_t cap, const char *name) {
    size_t name_len = strlen(name);
    size_t total = 1 + 4 + 1 + name_len + 1;
    if (cap < total) return -1;
    buf[0] = 'C';
    put_i32(buf + 1, (int32_t)(4 + 1 + name_len + 1));
    buf[5] = 'S';
    memcpy(buf + 6, name, name_len);
    buf[6 + name_len] = '\0';
    return (int)total;
}

/* -------------------------------------------------------------------------
 * Backend message parser
 * ---------------------------------------------------------------------- */

int pg_parse_message(const char *buf, size_t len, pg_parsed_msg *out) {
    if (len < 5) return 0;                      /* need tag + 4-byte length */

    char     tag    = buf[0];
    int32_t  msglen = get_i32(buf + 1);
    if (msglen < 4) return -1;                  /* invalid length */

    size_t total = 1 + (size_t)msglen;
    if (len < total) return 0;                  /* wait for more data */

    const char *body    = buf + 5;
    size_t      bodylen = (size_t)msglen - 4;

    out->type = PG_MSG_UNKNOWN;

    switch ((unsigned char)tag) {

    /* Authentication */
    case 'R': {
        if (bodylen < 4) return -1;
        int32_t authtype = get_i32(body);
        switch (authtype) {
        case 0:  out->type = PG_MSG_AUTH_OK;        break;
        case 3:  out->type = PG_MSG_AUTH_CLEARTEXT;  break;
        case 5:
            if (bodylen < 8) return -1;
            out->type = PG_MSG_AUTH_MD5;
            memcpy(out->md5.salt, body + 4, 4);
            break;
        case 10: {
            out->type = PG_MSG_AUTH_SASL;
            /* copy first mechanism name */
            size_t copy = bodylen - 4;
            if (copy > sizeof(out->sasl.sasl_mechanisms) - 1)
                copy = sizeof(out->sasl.sasl_mechanisms) - 1;
            memcpy(out->sasl.sasl_mechanisms, body + 4, copy);
            out->sasl.sasl_mechanisms[copy] = '\0';
            /* null-terminate at first \0 */
            char *nul = memchr(out->sasl.sasl_mechanisms, '\0',
                               sizeof(out->sasl.sasl_mechanisms));
            if (nul) *nul = '\0';
            break;
        }
        default: out->type = PG_MSG_UNKNOWN; break;
        }
        break;
    }

    /* RowDescription */
    case 'T': {
        if (bodylen < 2) return -1;
        int16_t ncols = get_i16(body);
        int16_t stored = (ncols < (int16_t)PG_MAX_COLS)
                       ? ncols : (int16_t)PG_MAX_COLS;
        out->row_description.ncols = stored;

        const char *p   = body + 2;
        const char *end = body + bodylen;
        for (int16_t i = 0; i < ncols; i++) {
            /* find null terminator for column name */
            const char *nul = memchr(p, '\0', (size_t)(end - p));
            if (!nul) return -1;
            size_t name_len = (size_t)(nul - p);
            if (i < stored) {
                size_t copy = name_len < 63 ? name_len : 63;
                memcpy(out->row_description.cols[i].name, p, copy);
                out->row_description.cols[i].name[copy] = '\0';
            }
            p = nul + 1 + 18;                   /* skip fixed descriptor fields */
            if (p > end) return -1;
        }
        out->type = PG_MSG_ROW_DESCRIPTION;
        break;
    }

    /* DataRow */
    case 'D': {
        if (bodylen < 2) return -1;
        out->type    = PG_MSG_DATA_ROW;
        out->data_row.nfields = get_i16(body);
        break;
    }

    /* CommandComplete */
    case 'C': {
        size_t copy = bodylen < sizeof(out->cmd_complete.tag) - 1
                    ? bodylen : sizeof(out->cmd_complete.tag) - 1;
        memcpy(out->cmd_complete.tag, body, copy);
        out->cmd_complete.tag[copy] = '\0';
        /* strip trailing \0 from server */
        char *end_nul = memchr(out->cmd_complete.tag, '\0',
                               sizeof(out->cmd_complete.tag));
        if (end_nul) *end_nul = '\0';
        out->type = PG_MSG_COMMAND_COMPLETE;
        break;
    }

    /* ReadyForQuery */
    case 'Z':
        out->type = PG_MSG_READY_FOR_QUERY;
        break;

    /* ErrorResponse / NoticeResponse */
    case 'E':
    case 'N': {
        out->type = (tag == 'E') ? PG_MSG_ERROR_RESPONSE : PG_MSG_NOTICE_RESPONSE;
        out->error.severity[0] = '\0';
        out->error.message[0]  = '\0';
        const char *p   = body;
        const char *end = body + bodylen;
        while (p < end) {
            char field_type = *p++;
            if (field_type == '\0') break;
            const char *val_end = memchr(p, '\0', (size_t)(end - p));
            if (!val_end) break;
            size_t val_len = (size_t)(val_end - p);
            if (field_type == 'S' && out->error.severity[0] == '\0') {
                size_t copy = val_len < sizeof(out->error.severity) - 1
                            ? val_len : sizeof(out->error.severity) - 1;
                memcpy(out->error.severity, p, copy);
                out->error.severity[copy] = '\0';
            } else if (field_type == 'M') {
                size_t copy = val_len < sizeof(out->error.message) - 1
                            ? val_len : sizeof(out->error.message) - 1;
                memcpy(out->error.message, p, copy);
                out->error.message[copy] = '\0';
            }
            p = val_end + 1;
        }
        break;
    }

    /* ParameterStatus */
    case 'S':
        out->type = PG_MSG_PARAMETER_STATUS;
        break;

    /* BackendKeyData */
    case 'K':
        if (bodylen >= 8) {
            out->backend_key.pid = (uint32_t)get_i32(body);
            out->backend_key.key = (uint32_t)get_i32(body + 4);
        }
        out->type = PG_MSG_BACKEND_KEY_DATA;
        break;

    /* P6-2 simple responses */
    case '1': out->type = PG_MSG_PARSE_COMPLETE;        break;
    case '2': out->type = PG_MSG_BIND_COMPLETE;         break;
    case '3': out->type = PG_MSG_CLOSE_COMPLETE;        break;
    case 'n': out->type = PG_MSG_NO_DATA;               break;

    /* ParameterDescription */
    case 't':
        out->type = PG_MSG_PARAMETER_DESCRIPTION;
        out->param_description.n_params = (bodylen >= 2) ? get_i16(body) : 0;
        break;

    default:
        out->type = PG_MSG_UNKNOWN;
        break;
    }

    return (int)total;
}
