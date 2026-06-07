/*
 * memcached protocol implementation (ADR 0005, Phase 4, t062).
 *
 * Implements the protocol vtable: connect (TCP, no auth handshake),
 * write (forward raw command bytes), readable (parse one text-protocol
 * reply), close (transport teardown + state free).
 *
 * memcached text protocol has no native TLS schema, so ssl_ctx is
 * accepted but ignored until a TLS transport profile is added.
 */

#include "memcached.h"
#include "mc_codec.h"
#include "wrkx_transport.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#define MC_RECVBUF        16384
#define MC_CONNECT_TIMEOUT_MS 5000

/* -------------------------------------------------------------------------
 * Module configuration (set once per process via memcached_configure)
 * ---------------------------------------------------------------------- */

static struct {
    struct addrinfo *addr;
    SSL_CTX         *ssl_ctx;
    const char      *host;
    const char      *password; /* reserved; memcached text has no AUTH */
} g_cfg;

void memcached_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                         const char *host, const char *password) {
    g_cfg.addr     = addr;
    g_cfg.ssl_ctx  = ssl_ctx;
    g_cfg.host     = host;
    g_cfg.password = password;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef struct {
    transport xport;
    char      rbuf[MC_RECVBUF];
    size_t    rbuf_len;
    int       done;
    size_t    bytes;
} mc_state;

/* -------------------------------------------------------------------------
 * vtable: connect
 * ---------------------------------------------------------------------- */

static int memcached_connect(connection *c) {
    mc_state *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    transport_init(&s->xport, g_cfg.addr, g_cfg.ssl_ctx, g_cfg.host);

    int fd = -1;
    if (transport_connect(&s->xport, &fd) != TRANSPORT_OK) {
        free(s);
        return -1;
    }

    /* Wait for the non-blocking connect to complete. */
    struct pollfd pfd = { fd, POLLOUT, 0 };
    if (poll(&pfd, 1, MC_CONNECT_TIMEOUT_MS) <= 0) {
        transport_close(&s->xport);
        free(s);
        return -1;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
        transport_close(&s->xport);
        free(s);
        return -1;
    }

    c->fd          = fd;
    c->proto_state = s;
    return 0;
}

/* -------------------------------------------------------------------------
 * vtable: write
 * ---------------------------------------------------------------------- */

static int memcached_write(connection *c, const char *buf, size_t len) {
    mc_state *s = c->proto_state;
    if (!s) return -1;

    switch (transport_handshake(&s->xport)) {
        case TRANSPORT_OK:    break;
        case TRANSPORT_RETRY: return 0;
        default:              return -1;
    }

    /* Reset per-request state on each new write cycle. */
    if (s->done) {
        s->rbuf_len = 0;
        s->done     = 0;
        s->bytes    = 0;
    }

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

static proto_status memcached_readable(connection *c) {
    mc_state *s = c->proto_state;
    if (!s) return PROTO_ERROR;

    switch (transport_handshake(&s->xport)) {
        case TRANSPORT_OK:    break;
        case TRANSPORT_RETRY: return PROTO_PENDING;
        default:              return PROTO_ERROR;
    }

    /* Read available bytes into the receive buffer. */
    if (s->rbuf_len < sizeof(s->rbuf)) {
        size_t n = 0;
        switch (transport_read(&s->xport,
                               s->rbuf + s->rbuf_len,
                               sizeof(s->rbuf) - s->rbuf_len, &n)) {
            case TRANSPORT_OK:    s->rbuf_len += n; break;
            case TRANSPORT_RETRY: return PROTO_PENDING;
            default:              return PROTO_ERROR;
        }
    }

    /* Parse one complete reply. */
    mc_reply  reply;
    size_t    consumed = 0;
    mc_status ps = mc_parse_reply(s->rbuf, s->rbuf_len, &reply, &consumed);

    if (ps == MC_STATUS_PENDING) {
        /* Buffer full with no complete reply — can't make progress. */
        if (s->rbuf_len == sizeof(s->rbuf))
            return PROTO_ERROR;
        return PROTO_PENDING;
    }
    if (ps == MC_STATUS_ERROR)   return PROTO_ERROR;

    /* Consume bytes from the front of the buffer. */
    s->bytes = consumed;
    size_t remaining = s->rbuf_len - consumed;
    if (remaining > 0)
        memmove(s->rbuf, s->rbuf + consumed, remaining);
    s->rbuf_len = remaining;

    c->bytes = s->bytes;
    s->done  = 1;

    /* Surface protocol-level errors as STATUS_ERR so stats track them. */
    switch (reply.type) {
        case MC_REPLY_NOT_STORED:
        case MC_REPLY_NOT_FOUND:
        case MC_REPLY_CLIENT_ERR:
        case MC_REPLY_SERVER_ERR:
            return PROTO_DONE_STATUS_ERR;
        default:
            return PROTO_DONE;
    }
}

/* -------------------------------------------------------------------------
 * vtable: close
 * ---------------------------------------------------------------------- */

static void memcached_close(connection *c) {
    mc_state *s = c->proto_state;
    if (s) {
        transport_close(&s->xport);
        free(s);
        c->proto_state = NULL;
    }
    c->fd = -1;
}

/* -------------------------------------------------------------------------
 * Vtable instance and getter
 * ---------------------------------------------------------------------- */

static protocol g_memcached_protocol = {
    .name     = "memcached",
    .connect  = memcached_connect,
    .write    = memcached_write,
    .readable = memcached_readable,
    .close    = memcached_close,
};

protocol *memcached_protocol(void) {
    return &g_memcached_protocol;
}
