/*
 * postgres.c — PostgreSQL protocol vtable.
 *
 * ADR 0005, Phase 6 (P6-1 + P6-2 + P6-3).
 * Implements connect/write/readable/close against the PostgreSQL wire protocol.
 */

#include "postgres.h"
#include "pg_message.h"
#include "pg_result.h"
#include "pg_scram.h"
#include "wrkx_transport.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <openssl/ssl.h>

#define PG_RECVBUF            32768
#define PG_AUTH_TIMEOUT_MS    10000

#define PG_RETRY_ERRNO(e) \
    ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)

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
    transport     xport;
    char          rbuf[PG_RECVBUF];
    size_t        rbuf_len;
    pg_phase      phase;
    bool          done;
    bool          error;
    size_t        bytes;
    int32_t       row_count;
    uint8_t       pg_status;        /* ReadyForQuery status: 'I','T','E'     */
    int32_t       queries_pending;  /* Q + Sync count; wait for all RFQs    */

    pg_col_desc_t cols[PG_RESULT_MAX_COLS];
    int16_t       ncols_desc;

    struct {                        /* SCRAM transient state — connect() only */
        char    nonce[64];
        char    bare[256];          /* client-first-message-bare             */
        size_t  bare_len;
        uint8_t expected_server_sig[32];
    } scram;
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

static int ssl_sync_send_all(SSL *ssl, int fd,
                             const char *buf, size_t len) {
    size_t sent = 0;
    struct pollfd pfd = { .fd = fd };
    while (sent < len) {
        int n = SSL_write(ssl, buf + sent, (int)(len - sent));
        if (n > 0) { sent += (size_t)n; continue; }
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_WRITE)      pfd.events = POLLOUT;
        else if (err == SSL_ERROR_WANT_READ)  pfd.events = POLLIN;
        else                                  return -1;
        if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) return -1;
    }
    return 0;
}

static int pg_sync_send(pg_state *s, const char *buf, size_t len) {
    if (s->xport.ssl)
        return ssl_sync_send_all(s->xport.ssl, s->xport.fd, buf, len);
    return sync_send_all(s->xport.fd, buf, len);
}

static int pg_sync_recv_msg(pg_state *s,
                            char *rbuf, size_t rbuf_cap,
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
        if (*rbuf_len >= rbuf_cap) return -1;
        struct pollfd pfd = { .fd = s->xport.fd, .events = POLLIN };
        if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n;
        if (s->xport.ssl) {
            n = SSL_read(s->xport.ssl, rbuf + *rbuf_len,
                         (int)(rbuf_cap - *rbuf_len));
            if (n <= 0) {
                int err = SSL_get_error(s->xport.ssl, (int)n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    continue;
                return -1;
            }
        } else {
            n = recv(s->xport.fd, rbuf + *rbuf_len,
                     rbuf_cap - *rbuf_len, 0);
            if (n <= 0) return -1;
        }
        *rbuf_len += (size_t)n;
    }
}

/* -------------------------------------------------------------------------
 * count_rfq_expected — scan write buffer for Q and Sync tags
 * ---------------------------------------------------------------------- */

static uint32_t u32be(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
           ((uint32_t)u[2] <<  8) |  (uint32_t)u[3];
}

int32_t count_rfq_expected(const char *buf, size_t len) {
    int32_t count = 0;
    size_t  pos   = 0;
    while (pos + 5 <= len) {
        uint8_t  tag  = (uint8_t)buf[pos];
        uint32_t mlen = u32be(buf + pos + 1);
        if (tag == 'Q' || tag == 'S') count++;
        if (mlen < 4) break;
        pos += 1 + mlen;
    }
    return count > 0 ? count : 1;
}

/* -------------------------------------------------------------------------
 * vtable: connect
 * ---------------------------------------------------------------------- */

static int pg_connect(connection *c) {
    pg_state *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    bool use_tls = (g_cfg.ssl_ctx != NULL);

    if (use_tls) {
        /* Phase 1: TCP-only connect */
        transport_init(&s->xport, g_cfg.addr, NULL, g_cfg.host);
        if (transport_connect(&s->xport, &c->fd) != TRANSPORT_OK) goto fail;

        struct pollfd pfd = { .fd = s->xport.fd, .events = POLLOUT };
        if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) goto fail;
        int so_err = 0;
        socklen_t so_len = sizeof(so_err);
        getsockopt(s->xport.fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
        if (so_err != 0) goto fail;

        /* Phase 2: SSLRequest prelude */
        static const uint8_t ssl_request[8] = {
            0, 0, 0, 8, 0x04, 0xd2, 0x16, 0x2f
        };
        size_t sent = 0;
        while (sent < 8) {
            ssize_t n = send(s->xport.fd,
                             ssl_request + sent, 8 - sent, 0);
            if (n > 0) { sent += (size_t)n; continue; }
            if (n < 0 && PG_RETRY_ERRNO(errno)) {
                if (errno != EINTR) {
                    pfd.events = POLLOUT;
                    if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) goto fail;
                }
                continue;
            }
            goto fail;
        }

        uint8_t ssl_resp = 0;
        for (;;) {
            ssize_t n = recv(s->xport.fd, &ssl_resp, 1, 0);
            if (n == 1) break;
            if (n < 0 && PG_RETRY_ERRNO(errno)) {
                if (errno != EINTR) {
                    pfd.events = POLLIN;
                    if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) goto fail;
                }
                continue;
            }
            goto fail;
        }
        if (ssl_resp != 'S') {
            fprintf(stderr,
                    "postgres: server rejected SSLRequest (got '%c')\n",
                    (char)ssl_resp);
            goto fail;
        }

        /* Phase 3: TLS handshake */
        SSL *ssl = SSL_new(g_cfg.ssl_ctx);
        if (!ssl) goto fail;
        SSL_set_fd(ssl, s->xport.fd);
        if (g_cfg.host)
            SSL_set_tlsext_host_name(ssl, g_cfg.host);
        for (;;) {
            int r = SSL_connect(ssl);
            if (r == 1) break;
            int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ)       pfd.events = POLLIN;
            else if (err == SSL_ERROR_WANT_WRITE) pfd.events = POLLOUT;
            else { SSL_free(ssl); goto fail; }
            if (poll(&pfd, 1, PG_AUTH_TIMEOUT_MS) <= 0) {
                SSL_free(ssl);
                goto fail;
            }
        }

        s->xport.ssl         = ssl;
        s->xport.ssl_ctx     = g_cfg.ssl_ctx;
        s->xport.handshaking = false;

    } else {
        transport_init(&s->xport, g_cfg.addr, NULL, g_cfg.host);
        if (transport_connect(&s->xport, &c->fd) != TRANSPORT_OK) goto fail;
    }

    /* Startup message */
    char sbuf[512];
    int n = pg_encode_startup(sbuf, sizeof(sbuf), g_cfg.user, g_cfg.dbname);
    if (n <= 0 || pg_sync_send(s, sbuf, (size_t)n) != 0) goto fail;

    /* Auth handshake loop */
    char rbuf[4096];
    size_t rbuf_len = 0;

    for (;;) {
        pg_parsed_msg msg;
        if (pg_sync_recv_msg(s, rbuf, sizeof(rbuf), &rbuf_len, &msg) != 0)
            goto fail;

        switch (msg.type) {
        case PG_MSG_AUTH_OK:
            memset(&s->scram, 0, sizeof(s->scram));
            break;

        case PG_MSG_AUTH_CLEARTEXT:
            if (!g_cfg.password) {
                fprintf(stderr, "postgres: server requires password\n");
                goto fail;
            }
            n = pg_encode_password(sbuf, sizeof(sbuf), g_cfg.password);
            if (n <= 0 || pg_sync_send(s, sbuf, (size_t)n) != 0) goto fail;
            break;

        case PG_MSG_AUTH_MD5:
            if (!g_cfg.password) {
                fprintf(stderr, "postgres: server requires password\n");
                goto fail;
            }
            n = pg_encode_md5_password(sbuf, sizeof(sbuf),
                                       g_cfg.password, g_cfg.user,
                                       msg.md5.salt);
            if (n <= 0 || pg_sync_send(s, sbuf, (size_t)n) != 0) goto fail;
            break;

        case PG_MSG_AUTH_SASL: {
            bool found_plain = false, found_plus = false;
            const char *p   = msg.sasl.list;
            const char *end = p + msg.sasl.len;
            while (p < end && *p) {
                if (!strcmp(p, "SCRAM-SHA-256"))      found_plain = true;
                if (!strcmp(p, "SCRAM-SHA-256-PLUS")) found_plus  = true;
                p += strlen(p) + 1;
            }
            if (!found_plain) {
                if (found_plus)
                    fprintf(stderr, "postgres: server requires "
                            "SCRAM-SHA-256-PLUS (channel binding); "
                            "configure plain SCRAM or wait for PLUS "
                            "support\n");
                else
                    fprintf(stderr,
                            "postgres: no supported SASL mechanism\n");
                goto fail;
            }
            n = pg_scram_client_first(g_cfg.user,
                                      s->scram.nonce,
                                      s->scram.bare,
                                      sizeof(s->scram.bare));
            if (n <= 0) goto fail;
            s->scram.bare_len = (size_t)n;

            char cf_full[512];
            if (3 + s->scram.bare_len >= sizeof(cf_full)) goto fail;
            memcpy(cf_full, "n,,", 3);
            memcpy(cf_full + 3, s->scram.bare, s->scram.bare_len);

            int enc = pg_encode_sasl_initial_response(
                sbuf, sizeof(sbuf),
                "SCRAM-SHA-256",
                cf_full, 3 + s->scram.bare_len);
            if (enc <= 0 || pg_sync_send(s, sbuf, (size_t)enc) != 0)
                goto fail;
            break;
        }

        case PG_MSG_AUTH_SASL_CONTINUE: {
            if (!g_cfg.password) {
                fprintf(stderr, "postgres: SCRAM auth requires a password "
                        "(add :password to the URL)\n");
                goto fail;
            }
            char final_buf[1024];
            uint8_t exp_sig[32];
            int flen = pg_scram_client_final(
                msg.sasl_continue.data, msg.sasl_continue.len,
                s->scram.nonce,
                s->scram.bare, s->scram.bare_len,
                g_cfg.password,
                exp_sig,
                final_buf, sizeof(final_buf));
            if (flen <= 0) goto fail;
            memcpy(s->scram.expected_server_sig, exp_sig, 32);

            int enc = pg_encode_sasl_response(sbuf, sizeof(sbuf),
                                              final_buf, (size_t)flen);
            if (enc <= 0 || pg_sync_send(s, sbuf, (size_t)enc) != 0)
                goto fail;
            break;
        }

        case PG_MSG_AUTH_SASL_FINAL:
            if (!pg_scram_verify_server(msg.sasl_final.data,
                                        msg.sasl_final.len,
                                        s->scram.expected_server_sig)) {
                fprintf(stderr,
                        "postgres: SCRAM server verification failed\n");
                goto fail;
            }
            break;

        case PG_MSG_PARAMETER_STATUS:
        case PG_MSG_BACKEND_KEY_DATA:
        case PG_MSG_NOTICE_RESPONSE:
            break;

        case PG_MSG_READY_FOR_QUERY:
            s->phase = PG_PHASE_READY;
            goto connected;

        case PG_MSG_ERROR_RESPONSE:
            fprintf(stderr, "postgres: connection error: %s\n",
                    msg.error.message);
            goto fail;

        default:
            break;
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

    /* New request cycle — guard against partial-write continuation calls */
    if (s->phase == PG_PHASE_READY) {
        pg_result_reset();
        s->done            = false;
        s->error           = false;
        s->bytes           = 0;
        s->row_count       = 0;
        s->ncols_desc      = 0;
        s->queries_pending = count_rfq_expected(buf, len);
        s->phase           = PG_PHASE_QUERY;
    }

    if (len == 0) return 0;

    size_t nw = 0;
    switch (transport_write(&s->xport, buf, len, &nw)) {
    case TRANSPORT_OK:    return (int)nw;
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

    bool do_done = false;

    for (;;) {
        pg_parsed_msg msg;
        int rc = pg_parse_message(s->rbuf, s->rbuf_len, &msg);
        if (rc == 0) break;
        if (rc < 0) return PROTO_ERROR;

        size_t consumed = (size_t)rc;

        /* Process BEFORE memmove so data_row field pointers remain valid */
        switch (msg.type) {
        case PG_MSG_READY_FOR_QUERY:
            s->pg_status = msg.rfq.status;
            if (--s->queries_pending <= 0) {
                s->done              = true;
                s->phase             = PG_PHASE_READY;
                tls_result.valid     = true;
                tls_result.pg_status = s->pg_status;
                do_done = true;
            }
            s->bytes += consumed;
            break;

        case PG_MSG_ROW_DESCRIPTION: {
            int16_t raw = msg.row_description.ncols;
            int16_t nc  = (raw > (int16_t)PG_RESULT_MAX_COLS)
                        ? (int16_t)PG_RESULT_MAX_COLS : raw;
            s->ncols_desc = nc;
            memcpy(s->cols, msg.row_description.cols,
                   (size_t)nc * sizeof(pg_col_desc_t));
            pg_result_set_columns(s->cols, nc);
            s->bytes += consumed;
            break;
        }

        case PG_MSG_DATA_ROW:
            pg_result_append_row(s->cols, s->ncols_desc,
                                 &msg.data_row, consumed);
            s->bytes += consumed;
            s->row_count++;
            break;

        case PG_MSG_COMMAND_COMPLETE:
            pg_result_set_cmd_tag(msg.cmd_complete.tag);
            s->bytes += consumed;
            break;

        case PG_MSG_ERROR_RESPONSE:
            s->error = true;
            break;

        case PG_MSG_PARSE_COMPLETE:
        case PG_MSG_BIND_COMPLETE:
        case PG_MSG_CLOSE_COMPLETE:
        case PG_MSG_PARAMETER_DESCRIPTION:
            break;

        case PG_MSG_NO_DATA:
            s->ncols_desc = 0;
            break;

        case PG_MSG_PARAMETER_STATUS:
        case PG_MSG_NOTICE_RESPONSE:
            break;

        default:
            break;
        }

        s->rbuf_len -= consumed;
        memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);

        if (do_done) goto done;
    }

    return PROTO_PENDING;

done:
    c->bytes = s->bytes;
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
