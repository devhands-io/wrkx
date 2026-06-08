/*
 * postgres.c — PostgreSQL protocol vtable.
 *
 * ADR 0005, Phase 6 (P6-1 + P6-2).
 * Implements connect/write/readable/close against the PostgreSQL wire protocol.
 */

#include "postgres.h"
#include "pg_message.h"
#include "wrkx_transport.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>

#define PG_RECVBUF            32768
#define PG_AUTH_TIMEOUT_MS    10000
#define PG_MAX_COLS           64

/* -------------------------------------------------------------------------
 * Module configuration (set once before any connections)
 * ---------------------------------------------------------------------- */

static struct {
    struct addrinfo *addr;
    SSL_CTX         *ssl_ctx;
    const char      *host;
    const char      *user;
    const char      *password;
    const char      *dbname;
} g_cfg;

void postgres_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                        const char *host,
                        const char *user, const char *password,
                        const char *dbname) {
    g_cfg.addr     = addr;
    g_cfg.ssl_ctx  = ssl_ctx;
    g_cfg.host     = host;
    g_cfg.user     = user;
    g_cfg.password = password;
    g_cfg.dbname   = dbname;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef enum {
    PG_PHASE_STARTUP,
    PG_PHASE_READY,
    PG_PHASE_QUERY,
    PG_PHASE_ERROR,
} pg_phase;

typedef struct {
    char name[64];
} pg_col_info;

typedef struct {
    transport    xport;
    char         rbuf[PG_RECVBUF];
    size_t       rbuf_len;
    pg_phase     phase;
    bool         done;
    bool         error;
    size_t       bytes;
    int32_t      row_count;
    pg_col_info  columns[PG_MAX_COLS];
    int16_t      n_cols;
} pg_state;

/* -------------------------------------------------------------------------
 * Synchronous helpers used during connect()
 * ---------------------------------------------------------------------- */

static int sync_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/*
 * Read and parse the next complete backend message from fd into rbuf.
 * Blocks up to PG_AUTH_TIMEOUT_MS per recv call.
 */
static int sync_recv_pg_message(int fd, char *rbuf, size_t rbuf_cap,
                                size_t *rbuf_len, pg_parsed_msg *out) {
    for (;;) {
        int rc = pg_parse_message(rbuf, *rbuf_len, out);
        if (rc > 0) {
            size_t consumed = (size_t)rc;
            *rbuf_len -= consumed;
            memmove(rbuf, rbuf + consumed, *rbuf_len);
            return 0;
        }
        if (rc < 0) return -1;
        /* need more data */
        if (*rbuf_len >= rbuf_cap) return -1;
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = recv(fd, rbuf + *rbuf_len, rbuf_cap - *rbuf_len, 0);
        if (n <= 0) return -1;
        *rbuf_len += (size_t)n;
    }
}

/* -------------------------------------------------------------------------
 * vtable: connect
 * ---------------------------------------------------------------------- */

static int pg_connect(connection *c) {
    pg_state *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    transport_init(&s->xport, g_cfg.addr, g_cfg.ssl_ctx, g_cfg.host);

    int fd = -1;
    if (transport_connect(&s->xport, &fd) != TRANSPORT_OK) {
        free(s);
        return -1;
    }

    /* Send startup message */
    char sbuf[512];
    int n = pg_encode_startup(sbuf, sizeof(sbuf), g_cfg.user, g_cfg.dbname);
    if (n <= 0 || sync_send_all(fd, sbuf, (size_t)n) != 0)
        goto fail;

    /* Auth handshake loop — exit only on READY_FOR_QUERY */
    char rbuf[4096];
    size_t rbuf_len = 0;

    for (;;) {
        pg_parsed_msg msg;
        if (sync_recv_pg_message(fd, rbuf, sizeof(rbuf),
                                 &rbuf_len, &msg) != 0)
            goto fail;

        switch (msg.type) {
        case PG_MSG_AUTH_OK:
            /* do NOT break out; server still sends ParameterStatus* +
             * BackendKeyData + ReadyForQuery after this */
            break;

        case PG_MSG_AUTH_CLEARTEXT:
            if (!g_cfg.password) {
                fprintf(stderr, "pg: server requires password\n");
                goto fail;
            }
            n = pg_encode_password(sbuf, sizeof(sbuf), g_cfg.password);
            if (n <= 0 || sync_send_all(fd, sbuf, (size_t)n) != 0)
                goto fail;
            break;

        case PG_MSG_AUTH_MD5:
            if (!g_cfg.password) {
                fprintf(stderr, "pg: server requires password\n");
                goto fail;
            }
            n = pg_encode_md5_password(sbuf, sizeof(sbuf),
                                       g_cfg.password, g_cfg.user,
                                       msg.md5.salt);
            if (n <= 0 || sync_send_all(fd, sbuf, (size_t)n) != 0)
                goto fail;
            break;

        case PG_MSG_AUTH_SASL:
            fprintf(stderr,
                "pg: SCRAM-SHA-256 auth not supported in P6-1; "
                "use md5 or trust in pg_hba.conf\n");
            goto fail;

        case PG_MSG_PARAMETER_STATUS:
        case PG_MSG_BACKEND_KEY_DATA:
        case PG_MSG_NOTICE_RESPONSE:
            break; /* ignore */

        case PG_MSG_READY_FOR_QUERY:
            s->phase = PG_PHASE_READY;
            goto connected;

        case PG_MSG_ERROR_RESPONSE:
            fprintf(stderr, "pg: connection error: %s\n", msg.error.message);
            goto fail;

        default:
            break; /* ignore unexpected */
        }
    }

connected:
    c->proto_state = s;
    return 0;

fail:
    transport_close(&s->xport);
    free(s);
    return -1;
}

/* -------------------------------------------------------------------------
 * vtable: write
 * ---------------------------------------------------------------------- */

static int pg_write(connection *c, const char *buf, size_t len) {
    pg_state *s = c->proto_state;
    if (!s) return -1;

    switch (transport_handshake(&s->xport)) {
    case TRANSPORT_OK:    break;
    case TRANSPORT_RETRY: return 0;
    default:              return -1;
    }

    /* Reset per-request state */
    s->done      = false;
    s->error     = false;
    s->bytes     = 0;
    s->row_count = 0;
    s->n_cols    = 0;
    s->phase     = PG_PHASE_QUERY;

    if (len == 0) return 0;

    size_t n = 0;
    switch (transport_write(&s->xport, buf, len, &n)) {
    case TRANSPORT_OK:    return (int)n;
    case TRANSPORT_RETRY: return 0;
    default:              return -1;
    }
}

/* -------------------------------------------------------------------------
 * vtable: readable
 * ---------------------------------------------------------------------- */

static proto_status pg_readable(connection *c) {
    pg_state *s = c->proto_state;
    if (!s) return PROTO_ERROR;

    switch (transport_handshake(&s->xport)) {
    case TRANSPORT_OK:    break;
    case TRANSPORT_RETRY: return PROTO_PENDING;
    default:              return PROTO_ERROR;
    }

    if (s->rbuf_len < sizeof(s->rbuf)) {
        size_t n = 0;
        switch (transport_read(&s->xport,
                               s->rbuf + s->rbuf_len,
                               sizeof(s->rbuf) - s->rbuf_len,
                               &n)) {
        case TRANSPORT_OK:    s->rbuf_len += n; break;
        case TRANSPORT_RETRY: return PROTO_PENDING;
        default:              return PROTO_ERROR;
        }
    }

    for (;;) {
        pg_parsed_msg msg;
        int rc = pg_parse_message(s->rbuf, s->rbuf_len, &msg);
        if (rc == 0) break;
        if (rc < 0) return PROTO_ERROR;

        size_t consumed = (size_t)rc;
        s->rbuf_len -= consumed;
        memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);

        switch (msg.type) {
        case PG_MSG_READY_FOR_QUERY:
            s->done    = true;
            c->bytes   = s->bytes;
            goto done;

        case PG_MSG_ROW_DESCRIPTION: {
            int16_t nc = msg.row_description.ncols;
            s->n_cols = (nc < (int16_t)PG_MAX_COLS) ? nc : (int16_t)PG_MAX_COLS;
            memcpy(s->columns, msg.row_description.cols,
                   (size_t)s->n_cols * sizeof(s->columns[0]));
            break;
        }

        case PG_MSG_DATA_ROW:
            s->bytes += consumed;
            s->row_count++;
            break;

        case PG_MSG_COMMAND_COMPLETE:
            s->bytes += consumed;
            break;

        case PG_MSG_ERROR_RESPONSE:
            s->error = true;
            break; /* keep reading to READY_FOR_QUERY */

        /* P6-2 extended-query responses */
        case PG_MSG_PARSE_COMPLETE:
        case PG_MSG_BIND_COMPLETE:
        case PG_MSG_CLOSE_COMPLETE:
        case PG_MSG_PARAMETER_DESCRIPTION:
            break; /* consume */

        case PG_MSG_NO_DATA:
            s->n_cols = 0;
            break;

        case PG_MSG_PARAMETER_STATUS:
        case PG_MSG_NOTICE_RESPONSE:
            break; /* can arrive mid-query */

        default:
            break;
        }
    }

    return PROTO_PENDING;

done:
    return s->error ? PROTO_DONE_STATUS_ERR : PROTO_DONE;
}

/* -------------------------------------------------------------------------
 * vtable: close
 * ---------------------------------------------------------------------- */

static void pg_close(connection *c) {
    pg_state *s = c->proto_state;
    if (!s) return;
    transport_close(&s->xport);
    free(s);
    c->proto_state = NULL;
}

/* -------------------------------------------------------------------------
 * Public vtable
 * ---------------------------------------------------------------------- */

static protocol pg_proto = {
    .name     = "postgres",
    .connect  = pg_connect,
    .write    = pg_write,
    .readable = pg_readable,
    .close    = pg_close,
};

protocol *postgres_protocol(void) { return &pg_proto; }
