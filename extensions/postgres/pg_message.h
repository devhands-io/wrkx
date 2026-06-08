#ifndef PG_MESSAGE_H
#define PG_MESSAGE_H

/*
 * PostgreSQL wire-protocol codec — backend message parser and frontend
 * message encoder.  No networking, no wrkx engine headers.
 *
 * ADR 0005, Phase 6 (P6-1 + P6-2).
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
    PG_MSG_AUTH_SASL,            /* 'R', authtype=10 — parsed, rejected       */
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
 * Parsed message struct
 * ---------------------------------------------------------------------- */

#define PG_MAX_COLS 64

typedef struct pg_parsed_msg {
    pg_msg_type type;
    union {
        struct { uint8_t salt[4]; }              md5;
        struct { char tag[64]; }                 cmd_complete;
        struct { char severity[16];
                 char message[256]; }            error;
        struct {
            int16_t ncols;                      /* clamped to PG_MAX_COLS    */
            struct { char name[64]; } cols[PG_MAX_COLS];
        }                                        row_description;
        struct { int16_t nfields; }              data_row;
        struct { char sasl_mechanisms[128]; }    sasl;
        struct { uint32_t pid; uint32_t key; }   backend_key;
        struct { int16_t n_params; }             param_description;
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

#endif /* PG_MESSAGE_H */
