#ifndef PG_MESSAGE_H
#define PG_MESSAGE_H

/*
 * PostgreSQL wire-protocol codec — backend message parser and frontend
 * message encoder.  No networking, no wrkx engine headers.
 *
 * ADR 0005, Phase 6 (P6-1 + P6-2 + P6-3).
 */

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Backend message types
 * ---------------------------------------------------------------------- */

typedef enum {
    /* P6-1 types */
    PG_MSG_UNKNOWN = 0,
    PG_MSG_AUTH_OK,              /* 'R', authtype=0                           */
    PG_MSG_AUTH_CLEARTEXT,       /* 'R', authtype=3                           */
    PG_MSG_AUTH_MD5,             /* 'R', authtype=5 — carries 4-byte salt     */
    PG_MSG_AUTH_SASL,            /* 'R', authtype=10 — NUL-separated list     */
    PG_MSG_AUTH_SASL_CONTINUE,   /* 'R', authtype=11 — server-first-message   */
    PG_MSG_AUTH_SASL_FINAL,      /* 'R', authtype=12 — server-final-message   */
    PG_MSG_ROW_DESCRIPTION,      /* 'T'                                       */
    PG_MSG_DATA_ROW,             /* 'D'                                       */
    PG_MSG_COMMAND_COMPLETE,     /* 'C'                                       */
    PG_MSG_READY_FOR_QUERY,      /* 'Z'                                       */
    PG_MSG_ERROR_RESPONSE,       /* 'E'                                       */
    PG_MSG_NOTICE_RESPONSE,      /* 'N'                                       */
    PG_MSG_PARAMETER_STATUS,     /* 'S'                                       */
    PG_MSG_BACKEND_KEY_DATA,     /* 'K'                                       */
    /* P6-2 additions */
    PG_MSG_PARSE_COMPLETE,       /* '1'                                       */
    PG_MSG_BIND_COMPLETE,        /* '2'                                       */
    PG_MSG_CLOSE_COMPLETE,       /* '3'                                       */
    PG_MSG_PARAMETER_DESCRIPTION,/* 't'                                       */
    PG_MSG_NO_DATA,              /* 'n'                                       */
} pg_msg_type;

/* -------------------------------------------------------------------------
 * Named types for result decoding (P6-3)
 * ---------------------------------------------------------------------- */

#define PG_RESULT_MAX_COLS  64
#define PG_MAX_COLS         PG_RESULT_MAX_COLS   /* backward-compat alias    */

/* Column descriptor from RowDescription — stored in pg_state.cols[] */
typedef struct {
    char    name[64];
    int32_t type_oid;
} pg_col_desc_t;

/* Decoded DataRow — field data pointers into the parse buffer;
 * callers must copy before the buffer shifts (e.g. before memmove). */
typedef struct {
    int16_t  nfields;
    struct {
        int32_t     len;    /* -1 for SQL NULL */
        const char *data;   /* points into parse buf; copy before buf shifts */
    } fields[PG_RESULT_MAX_COLS];
} pg_data_row_t;

/* -------------------------------------------------------------------------
 * Parsed message struct
 * ---------------------------------------------------------------------- */

typedef struct pg_parsed_msg {
    pg_msg_type type;
    union {
        struct { uint8_t salt[4]; }              md5;
        struct { char tag[64]; }                 cmd_complete;
        struct { char severity[16];
                 char message[256]; }            error;
        struct {
            int16_t       ncols;
            pg_col_desc_t cols[PG_RESULT_MAX_COLS];
        }                                        row_description;
        pg_data_row_t                            data_row;
        struct { char list[256]; size_t len; }   sasl;           /* full NUL-sep list */
        struct { char data[512]; size_t len; }   sasl_continue;  /* server-first      */
        struct { char data[256]; size_t len; }   sasl_final;     /* server-final      */
        struct { uint32_t pid; uint32_t key; }   backend_key;
        struct { int16_t n_params; }             param_description;
        struct { uint8_t status; }               rfq;            /* 'I','T','E'       */
    };
} pg_parsed_msg;

/*
 * Incremental backend-message parser.
 * Returns bytes consumed (> 0) on success, 0 if more data needed,
 * -1 on unrecoverable parse error.
 */
int pg_parse_message(const char *buf, size_t len, pg_parsed_msg *out);

/* -------------------------------------------------------------------------
 * Frontend message encoders
 *
 * All return bytes written (> 0) or <= 0 if the buffer is too small.
 * ---------------------------------------------------------------------- */

/* P6-1 */
int pg_encode_startup (char *buf, size_t cap,
                       const char *user, const char *db);
int pg_encode_query   (char *buf, size_t cap, const char *sql);
int pg_encode_password(char *buf, size_t cap, const char *password);
int pg_encode_md5_password(char *buf, size_t cap,
                           const char *password, const char *user,
                           const uint8_t salt[4]);

/* P6-2 */
int pg_encode_parse   (char *buf, size_t cap,
                       const char *name, const char *sql);
int pg_encode_bind    (char *buf, size_t cap,
                       const char *portal, const char *stmt,
                       const char * const *params,
                       const size_t *param_lens, int16_t n_params);
int pg_encode_describe(char *buf, size_t cap, char type, const char *name);
int pg_encode_execute (char *buf, size_t cap,
                       const char *portal, int32_t max_rows);
int pg_encode_sync    (char *buf, size_t cap);
int pg_encode_close_stmt(char *buf, size_t cap, const char *name);

/* P6-3 SASL */
/* 'p' + int32(len) + mechanism + '\0' + int32(cf_len) + client_first */
int pg_encode_sasl_initial_response(char *buf, size_t cap,
                                    const char *mechanism,
                                    const char *client_first, size_t cf_len);
/* 'p' + int32(len) + client_final (no null terminator) */
int pg_encode_sasl_response(char *buf, size_t cap,
                            const char *client_final, size_t cf_len);

#endif /* PG_MESSAGE_H */
