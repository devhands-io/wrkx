/*
 * Redis protocol implementation (ADR 0005, Phase 2, P2-1).
 *
 * Implements the `protocol` vtable: connect (TCP/optional-TLS + AUTH/SELECT
 * handshake), write (forward RESP command bytes), readable (parse RESP
 * response), close.
 *
 * Gate A invariant: this file must not modify orchestrator.c, proto.h, ae.c,
 * or rate.c. All protocol behaviour is encapsulated here.
 *
 * Invariant 2: no scripting header is included anywhere in this file.
 */

#include "proto/redis.h"
#include "proto/resp.h"
#include "transport.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#define REDIS_RECVBUF    16384
#define REDIS_AUTH_TIMEOUT_MS 5000

/* -------------------------------------------------------------------------
 * Module configuration
 * ---------------------------------------------------------------------- */

static struct {
    struct addrinfo *addr;
    SSL_CTX         *ssl_ctx;
    const char      *host;
    const char      *password;  /* NULL = no AUTH */
    int              db;        /* 0 = no SELECT  */
} g_cfg;

void redis_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                     const char *host, const char *password, int db) {
    g_cfg.addr     = addr;
    g_cfg.ssl_ctx  = ssl_ctx;
    g_cfg.host     = host;
    g_cfg.password = password;
    g_cfg.db       = db;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef struct redis_state {
    transport  xport;              /* TCP / optional TLS transport              */
    char       rbuf[REDIS_RECVBUF];/* response read buffer                     */
    size_t     rbuf_len;           /* bytes currently buffered                  */
    bool       done;               /* last readable() returned PROTO_DONE*      */
    bool       error;              /* last readable() returned PROTO_ERROR      */
    size_t     bytes;              /* wire bytes of the completed response      */
} redis_state;

/* -------------------------------------------------------------------------
 * Synchronous send/recv helpers for AUTH/SELECT in connect()
 *
 * The socket is non-blocking (set by transport_connect), but we drive it
 * with poll() so we can block for the AUTH RTT without changing socket flags.
 * This is intentionally blocking — AUTH happens once per connection and
 * typically completes in <1ms on localhost.
 * ---------------------------------------------------------------------- */

static int sync_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, REDIS_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/*
 * Read until resp_parse() finds a complete response or timeout.
 * Returns 0 on success (parsed), -1 on error/timeout.
 * On success, *reply is set to '+' or '-' (first byte of the response).
 */
static int sync_recv_resp(int fd, char *rbuf, size_t rbuf_cap,
                          char *reply_type) {
    size_t len = 0;
    for (;;) {
        size_t out = 0;
        int rc = resp_parse(rbuf, len, &out);
        if (rc > 0) {
            *reply_type = rbuf[0];
            return 0;
        }
        if (rc < 0) return -1;
        /* need more data */
        if (len >= rbuf_cap) return -1;  /* buffer full, no complete response */
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, REDIS_AUTH_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = recv(fd, rbuf + len, rbuf_cap - len, 0);
        if (n <= 0) return -1;
        len += (size_t)n;
    }
}

static int send_command_sync(int fd, int argc, const char * const *argv,
                             const size_t *arglens, char *reply_type) {
    char cmd[512];
    int n = resp_encode(cmd, sizeof(cmd), argc, argv, arglens);
    if (n <= 0) return -1;
    if (sync_send_all(fd, cmd, (size_t)n) != 0) return -1;
    char rbuf[256];
    return sync_recv_resp(fd, rbuf, sizeof(rbuf), reply_type);
}

/* -------------------------------------------------------------------------
 * vtable: connect
 * ---------------------------------------------------------------------- */

static int redis_connect(connection *c) {
    redis_state *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    transport_init(&s->xport, g_cfg.addr, g_cfg.ssl_ctx, g_cfg.host);

    int fd = -1;
    if (transport_connect(&s->xport, &fd) != TRANSPORT_OK) {
        free(s);
        return -1;
    }

    /* Wait for the non-blocking connect() to complete. */
    struct pollfd pfd = { fd, POLLOUT, 0 };
    if (poll(&pfd, 1, REDIS_AUTH_TIMEOUT_MS) <= 0) {
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

    /* AUTH handshake (synchronous, one RTT). */
    if (g_cfg.password != NULL) {
        const char *argv[2] = { "AUTH", g_cfg.password };
        size_t arglens[2]   = { 4, strlen(g_cfg.password) };
        char reply;
        if (send_command_sync(fd, 2, argv, arglens, &reply) != 0 ||
            reply != '+') {
            transport_close(&s->xport);
            free(s);
            return -1;
        }
    }

    /* SELECT <db> (synchronous, one RTT). */
    if (g_cfg.db > 0) {
        char dbstr[16];
        snprintf(dbstr, sizeof(dbstr), "%d", g_cfg.db);
        const char *argv[2] = { "SELECT", dbstr };
        size_t arglens[2]   = { 6, strlen(dbstr) };
        char reply;
        if (send_command_sync(fd, 2, argv, arglens, &reply) != 0 ||
            reply != '+') {
            transport_close(&s->xport);
            free(s);
            return -1;
        }
    }

    c->fd          = fd;
    c->proto_state = s;
    return 0;
}

/* -------------------------------------------------------------------------
 * vtable: write
 * ---------------------------------------------------------------------- */

static int redis_write(connection *c, const char *buf, size_t len) {
    redis_state *s = c->proto_state;
    if (!s) return -1;

    /* Drive TLS handshake on the first write (no-op for plain TCP). */
    switch (transport_handshake(&s->xport)) {
        case TRANSPORT_OK:    break;
        case TRANSPORT_RETRY: return 0;
        default:              return -1;
    }

    /* A new request starts after the previous response was completed.
     * Reset the response buffer so readable() parses the next reply fresh. */
    if (s->done || s->error) {
        s->rbuf_len = 0;
        s->done     = false;
        s->error    = false;
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

static proto_status redis_readable(connection *c) {
    redis_state *s = c->proto_state;
    if (!s) return PROTO_ERROR;

    /* Drive TLS handshake if still in flight. */
    switch (transport_handshake(&s->xport)) {
        case TRANSPORT_OK:    break;
        case TRANSPORT_RETRY: return PROTO_PENDING;
        default:              return PROTO_ERROR;
    }

    /* Read available bytes into the tail of rbuf. */
    if (s->rbuf_len < sizeof(s->rbuf)) {
        size_t n = 0;
        transport_status rs = transport_read(&s->xport,
                                             s->rbuf + s->rbuf_len,
                                             sizeof(s->rbuf) - s->rbuf_len,
                                             &n);
        switch (rs) {
            case TRANSPORT_OK:
                s->rbuf_len += n;
                break;
            case TRANSPORT_RETRY:
                return PROTO_PENDING;
            case TRANSPORT_EOF:
                /* Peer closed before a complete response — treat as error. */
                s->error = true;
                return PROTO_ERROR;
            default:
                s->error = true;
                return PROTO_ERROR;
        }
    }

    /* Attempt to parse one complete RESP response from the buffer. */
    size_t consumed = 0;
    int rc = resp_parse(s->rbuf, s->rbuf_len, &consumed);
    if (rc == 0) return PROTO_PENDING;
    if (rc < 0) {
        s->error = true;
        return PROTO_ERROR;
    }

    /* Classify: a RESP error reply ('-') maps to PROTO_DONE_STATUS_ERR. */
    proto_status result = (s->rbuf[0] == '-') ? PROTO_DONE_STATUS_ERR
                                               : PROTO_DONE;

    /* Surface wire size and consume the bytes from the buffer. */
    c->bytes = consumed;
    s->bytes = consumed;
    s->done  = true;

    size_t remaining = s->rbuf_len - consumed;
    if (remaining > 0)
        memmove(s->rbuf, s->rbuf + consumed, remaining);
    s->rbuf_len = remaining;

    return result;
}

/* -------------------------------------------------------------------------
 * vtable: close
 * ---------------------------------------------------------------------- */

static void redis_close(connection *c) {
    redis_state *s = c->proto_state;
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

static protocol redis = {
    .name     = "redis",
    .connect  = redis_connect,
    .write    = redis_write,
    .readable = redis_readable,
    .close    = redis_close,
};

protocol *redis_protocol(void) {
    return &redis;
}

/* -------------------------------------------------------------------------
 * Request-construction helper (used by Request Layer glue, not wire path)
 * ---------------------------------------------------------------------- */

char *redis_make_request(int argc, const char * const *argv,
                         const size_t *arglens, size_t *len_out) {
    if (argc <= 0 || !argv || !arglens) return NULL;

    /* Conservative estimate: header line + per-arg overhead + data. */
    size_t cap = 32;
    for (int i = 0; i < argc; i++)
        cap += 24 + arglens[i];

    char *buf = malloc(cap);
    if (!buf) return NULL;

    int n = resp_encode(buf, cap, argc, argv, arglens);
    if (n <= 0) { free(buf); return NULL; }

    if (len_out) *len_out = (size_t)n;
    return buf;
}
