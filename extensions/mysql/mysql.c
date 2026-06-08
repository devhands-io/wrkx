/*
 * mysql.c — MySQL Client/Server protocol vtable.
 *
 * Implements connect/write/readable/close for the MySQL 4.1+ protocol:
 * handshake, native_password / caching_sha2_password fast-path auth,
 * COM_QUERY request, and text result-set parsing.
 *
 * ADR 0005, Phase 6 (P6-4).
 */

#include "mysql.h"
#include "mysql_packet.h"
#include "wrkx_transport.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#define MYSQL_RECVBUF         65536
#define MYSQL_AUTH_TIMEOUT_MS 10000

/* -------------------------------------------------------------------------
 * Module configuration
 * ---------------------------------------------------------------------- */

static struct {
    struct addrinfo *addr;
    const char      *host;
    const char      *user;
    const char      *password;
    const char      *dbname;
} g_cfg;

void mysql_configure(struct addrinfo *addr,
                     const char *host,
                     const char *user, const char *password,
                     const char *dbname) {
    g_cfg.addr     = addr;
    g_cfg.host     = host;
    g_cfg.user     = user;
    g_cfg.password = password;
    g_cfg.dbname   = dbname;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef enum {
    MYSQL_PHASE_HANDSHAKE,
    MYSQL_PHASE_AUTH,
    MYSQL_PHASE_AUTH_SWITCH,
    MYSQL_PHASE_READY,
    MYSQL_PHASE_QUERY,
    MYSQL_PHASE_ERROR,
} mysql_phase;

typedef enum {
    MYSQL_RS_PREAMBLE,
    MYSQL_RS_COL_DEFS,
    MYSQL_RS_ROWS,
} mysql_rs_phase;

typedef struct {
    transport      xport;
    uint8_t        rbuf[MYSQL_RECVBUF];
    size_t         rbuf_len;
    mysql_phase    phase;
    mysql_rs_phase rs_phase;
    uint64_t       col_count;
    uint64_t       cols_seen;
    int32_t        row_count;
    bool           done;
    bool           error;
    size_t         bytes;
    uint8_t        server_challenge[21];   /* 20 usable bytes + NUL */
    char           auth_plugin[64];
} mysql_state;

/* -------------------------------------------------------------------------
 * Synchronous send/recv helpers used during connect()
 * ---------------------------------------------------------------------- */

static int mysql_sync_send(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, MYSQL_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Receive until mysql_parse_packet succeeds for the given context. */
static int mysql_sync_recv_pkt(mysql_state *s, mysql_ctx ctx,
                               mysql_parsed_pkt *out) {
    for (;;) {
        int rc = mysql_parse_packet(s->rbuf, s->rbuf_len, ctx, out);
        if (rc > 0) {
            size_t consumed = (size_t)rc;
            s->rbuf_len -= consumed;
            memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
            return 0;
        }
        if (rc < 0) return -1;
        /* need more data */
        if (s->rbuf_len >= MYSQL_RECVBUF) return -1;
        struct pollfd pfd = { s->xport.fd, POLLIN, 0 };
        if (poll(&pfd, 1, MYSQL_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = recv(s->xport.fd,
                         s->rbuf + s->rbuf_len,
                         MYSQL_RECVBUF - s->rbuf_len, 0);
        if (n <= 0) return -1;
        s->rbuf_len += (size_t)n;
    }
}

/* -------------------------------------------------------------------------
 * vtable: connect
 * ---------------------------------------------------------------------- */

static int mysql_connect(connection *c) {
    mysql_state *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    transport_init(&s->xport, g_cfg.addr, NULL, g_cfg.host);
    if (transport_connect(&s->xport, &c->fd) != TRANSPORT_OK) goto fail;

    /* Step 2: read HandshakeV10 */
    mysql_parsed_pkt pkt;
    if (mysql_sync_recv_pkt(s, MYSQL_CTX_AUTH, &pkt) != 0) goto fail;
    if (pkt.type != MYSQL_PKT_HANDSHAKE_V10) goto fail;

    /* Extract auth plugin data (20 bytes) and plugin name */
    memcpy(s->server_challenge, pkt.handshake.auth_plugin_data, 20);
    s->server_challenge[20] = '\0';
    size_t nlen = strnlen(pkt.handshake.auth_plugin_name,
                          sizeof(pkt.handshake.auth_plugin_name));
    if (nlen >= sizeof(s->auth_plugin)) nlen = sizeof(s->auth_plugin) - 1;
    memcpy(s->auth_plugin, pkt.handshake.auth_plugin_name, nlen);
    s->auth_plugin[nlen] = '\0';

    /* Step 3: compute auth response */
    uint8_t auth_resp[32];
    uint8_t auth_resp_len;

    const char *plugin = s->auth_plugin;
    if (strcmp(plugin, "mysql_native_password") == 0 || plugin[0] == '\0') {
        mysql_native_password(g_cfg.password ? g_cfg.password : "",
                              s->server_challenge, auth_resp);
        auth_resp_len = 20;
    } else if (strcmp(plugin, "caching_sha2_password") == 0) {
        mysql_sha2_password_fast(g_cfg.password ? g_cfg.password : "",
                                 s->server_challenge, auth_resp);
        auth_resp_len = 32;
    } else {
        fprintf(stderr, "mysql: unsupported auth plugin: %s\n", plugin);
        goto fail;
    }

    /* If no password, send empty auth response */
    const char *pw = g_cfg.password;
    if (!pw || !*pw) {
        auth_resp_len = 0;
    }

    /* Step 4: send HandshakeResponse (seq=1) */
    uint8_t hrbuf[512];
    int n = mysql_encode_handshake_response(
                hrbuf, sizeof(hrbuf),
                g_cfg.user    ? g_cfg.user    : "wrkx",
                g_cfg.dbname  ? g_cfg.dbname  : "",
                auth_resp, auth_resp_len,
                s->auth_plugin,
                MYSQL_CLIENT_FLAGS_P64);
    if (n <= 0) goto fail;
    if (mysql_sync_send(s->xport.fd, hrbuf, (size_t)n) != 0) goto fail;

    /* Step 5: auth response loop */
    int auth_switches = 0;
    for (;;) {
        if (mysql_sync_recv_pkt(s, MYSQL_CTX_AUTH, &pkt) != 0) goto fail;

        if (pkt.type == MYSQL_PKT_OK) {
            s->phase = MYSQL_PHASE_READY;
            break;
        }

        if (pkt.type == MYSQL_PKT_AUTH_MORE_DATA) {
            if (pkt.auth_more_data.marker == 0x03) {
                /* fast-path success; final OK follows */
                continue;
            }
            /* marker == 0x04: full exchange needed */
            fprintf(stderr,
                    "mysql: caching_sha2_password full exchange not "
                    "supported in P6-4; configure server with "
                    "mysql_native_password or use an already-cached user\n");
            goto fail;
        }

        if (pkt.type == MYSQL_PKT_AUTH_SWITCH_REQ) {
            if (++auth_switches > 1) {
                fprintf(stderr, "mysql: too many auth plugin switches\n");
                goto fail;
            }
            /* Update plugin + challenge */
            nlen = strnlen(pkt.auth_switch.plugin_name,
                           sizeof(pkt.auth_switch.plugin_name));
            if (nlen >= sizeof(s->auth_plugin)) nlen = sizeof(s->auth_plugin) - 1;
            memcpy(s->auth_plugin, pkt.auth_switch.plugin_name, nlen);
            s->auth_plugin[nlen] = '\0';

            /* Use new challenge (auth_data is already 20 bytes max in the parser) */
            memcpy(s->server_challenge, pkt.auth_switch.auth_data, 20);

            plugin = s->auth_plugin;
            if (strcmp(plugin, "mysql_native_password") == 0) {
                mysql_native_password(g_cfg.password ? g_cfg.password : "",
                                      s->server_challenge, auth_resp);
                auth_resp_len = (pw && *pw) ? 20 : 0;
            } else if (strcmp(plugin, "caching_sha2_password") == 0) {
                mysql_sha2_password_fast(g_cfg.password ? g_cfg.password : "",
                                         s->server_challenge, auth_resp);
                auth_resp_len = (pw && *pw) ? 32 : 0;
            } else {
                fprintf(stderr, "mysql: unsupported auth plugin after switch: %s\n",
                        plugin);
                goto fail;
            }

            /* Send response packet (seq = received_pkt.seq + 1) */
            uint8_t resp_hdr[4];
            mysql_write_pkt_header(resp_hdr, auth_resp_len,
                                   (uint8_t)(pkt.seq + 1));
            if (mysql_sync_send(s->xport.fd, resp_hdr, 4) != 0) goto fail;
            if (auth_resp_len > 0 &&
                mysql_sync_send(s->xport.fd, auth_resp, auth_resp_len) != 0)
                goto fail;
            continue;
        }

        if (pkt.type == MYSQL_PKT_ERR) {
            fprintf(stderr, "mysql: auth error [%u] %s\n",
                    pkt.err.error_code, pkt.err.message);
            goto fail;
        }

        /* Unknown packet during auth */
        goto fail;
    }

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

static int mysql_write(connection *c, const char *buf, size_t len) {
    mysql_state *s = (mysql_state *)c->proto_state;
    s->phase     = MYSQL_PHASE_QUERY;
    s->rs_phase  = MYSQL_RS_PREAMBLE;
    s->done      = false;
    s->error     = false;
    s->bytes     = 0;
    s->row_count = 0;
    s->col_count = 0;
    s->cols_seen = 0;

    size_t written;
    transport_status ts = transport_write(&s->xport, buf, len, &written);
    return (ts == TRANSPORT_OK) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * vtable: readable
 * ---------------------------------------------------------------------- */

static proto_status mysql_readable(connection *c) {
    mysql_state *s = (mysql_state *)c->proto_state;

    /* TLS guard (plain TCP: no-op) */
    transport_status ts = transport_handshake(&s->xport);
    if (ts == TRANSPORT_RETRY) return PROTO_PENDING;
    if (ts == TRANSPORT_ERROR) return PROTO_ERROR;

    /* Append data to receive buffer */
    size_t n;
    ts = transport_read(&s->xport, s->rbuf + s->rbuf_len,
                        MYSQL_RECVBUF - s->rbuf_len, &n);
    if (ts == TRANSPORT_RETRY) return PROTO_PENDING;
    if (ts == TRANSPORT_EOF || ts == TRANSPORT_ERROR) return PROTO_ERROR;
    s->rbuf_len += n;

    /* Parse loop */
    for (;;) {
        mysql_parsed_pkt pkt;
        int rc;
        size_t consumed;

        switch (s->rs_phase) {
        case MYSQL_RS_PREAMBLE:
            rc = mysql_parse_packet(s->rbuf, s->rbuf_len,
                                    MYSQL_CTX_GENERIC, &pkt);
            if (rc == 0) goto pending;
            if (rc < 0)  return PROTO_ERROR;
            consumed = (size_t)rc;
            s->bytes += consumed;
            if (pkt.type == MYSQL_PKT_COLUMN_COUNT) {
                s->col_count = pkt.column_count.count;
                s->rs_phase  = MYSQL_RS_COL_DEFS;
            } else if (pkt.type == MYSQL_PKT_OK) {
                s->done = true;
            } else if (pkt.type == MYSQL_PKT_ERR) {
                s->error = true;
                s->done  = true;
            } else {
                return PROTO_ERROR;
            }
            s->rbuf_len -= consumed;
            memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
            if (s->done) goto done;
            break;

        case MYSQL_RS_COL_DEFS:
            rc = mysql_parse_packet(s->rbuf, s->rbuf_len,
                                    MYSQL_CTX_COL_DEF, &pkt);
            if (rc == 0) goto pending;
            if (rc < 0)  return PROTO_ERROR;
            consumed = (size_t)rc;
            s->bytes += consumed;
            if (pkt.type == MYSQL_PKT_COLUMN_DEF) {
                s->cols_seen++;
            } else if (pkt.type == MYSQL_PKT_EOF) {
                s->rs_phase = MYSQL_RS_ROWS;
            } else if (pkt.type == MYSQL_PKT_ERR) {
                s->error = true;
                s->done  = true;
                s->rbuf_len -= consumed;
                memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
                goto done;
            }
            s->rbuf_len -= consumed;
            memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
            break;

        case MYSQL_RS_ROWS:
            rc = mysql_parse_packet(s->rbuf, s->rbuf_len,
                                    MYSQL_CTX_ROW, &pkt);
            if (rc == 0) goto pending;
            if (rc < 0)  return PROTO_ERROR;
            consumed = (size_t)rc;
            s->bytes += consumed;
            if (pkt.type == MYSQL_PKT_ROW) {
                s->row_count++;
            } else if (pkt.type == MYSQL_PKT_EOF) {
                s->done = true;
            } else if (pkt.type == MYSQL_PKT_ERR) {
                s->error = true;
                s->done  = true;
            }
            s->rbuf_len -= consumed;
            memmove(s->rbuf, s->rbuf + consumed, s->rbuf_len);
            if (s->done) goto done;
            break;
        }
    }

done:
    c->bytes = s->bytes;
    return s->error ? PROTO_DONE_STATUS_ERR : PROTO_DONE;

pending:
    return PROTO_PENDING;
}

/* -------------------------------------------------------------------------
 * vtable: close
 * ---------------------------------------------------------------------- */

static void mysql_close(connection *c) {
    mysql_state *s = (mysql_state *)c->proto_state;
    if (!s) return;

    /* Best-effort COM_QUIT */
    uint8_t quit[5];
    int n = mysql_encode_com_quit(quit, sizeof(quit));
    if (n > 0) {
        size_t ignored;
        transport_write(&s->xport, quit, (size_t)n, &ignored);
    }

    transport_close(&s->xport);
    free(s);
    c->proto_state = NULL;
}

/* -------------------------------------------------------------------------
 * Protocol vtable
 * ---------------------------------------------------------------------- */

static protocol mysql_proto = {
    .name     = "mysql",
    .connect  = mysql_connect,
    .write    = mysql_write,
    .readable = mysql_readable,
    .close    = mysql_close,
};

protocol *mysql_protocol(void) {
    return &mysql_proto;
}
