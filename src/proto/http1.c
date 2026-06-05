/*
 * HTTP/1.1 protocol implementation (ADR 0001, Phase 1, Protocol Engine, P1-3).
 *
 * Implements the `protocol` vtable: connect (TCP/TLS), write (send request
 * bytes), readable (parse response, detect completion), close.
 *
 * Migrated from wrk.c: the http_parser callbacks (header_field, header_value,
 * response_body, response_complete), parser_settings, the read loop in
 * socket_readable and the connect/reconnect socket handling (the transport part
 * now lives in transport.c).
 *
 * --------------------------------------------------------------------------
 * Where the frozen contract felt insufficient (flagged, NOT changed):
 *
 * proto.h's `protocol.connect` receives only a `connection *`, and `struct
 * connection` carries only { fd, proto_state, script_state }. There is nowhere
 * in the contract to pass the connect target (resolved addrinfo, TLS context,
 * SNI host) or to surface byte counts / HTTP status back to the orchestrator.
 *
 * Resolution without touching the contract:
 *   - Connect target is supplied process-wide via http1_configure() and held in
 *     a module-static `g_cfg`. One CLI invocation == one target, so this mirrors
 *     wrk.c's single global `sock`/`cfg.host`/`cfg.ctx`. Per-connection live
 *     resources (socket, TLS object, parser, buffers) live in proto_state.
 *   - Response byte counts ARE surfaced (t042): connection.bytes was added to
 *     the contract as a one-word channel that the protocol fills with the wire
 *     size of each completed response. (The original Phase-1 note here claimed
 *     no channel was needed; that turned out to be wrong — Transfer/sec read
 *     0.00B without it.) HTTP status is still not surfaced; the orchestrator
 *     asks the Request Layer for status, so no extra channel is needed for it.
 *
 * Invariant 2: no scripting header is included anywhere in this file.
 * --------------------------------------------------------------------------
 */

#include "proto/http1.h"
#include "transport.h"
#include "http_parser.h"

#include <stdlib.h>
#include <string.h>

/* Read chunk size for each transport_read (matches wrk.c's RECVBUF). */
#define HTTP1_RECVBUF 8192

/* ------------------------------------------------------------------------- */
/* Module configuration (the connect target; see contract note above)        */
/* ------------------------------------------------------------------------- */

static struct {
    struct addrinfo *addr;
    SSL_CTX         *ssl_ctx;
    const char      *host;
} g_cfg;

void http1_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                     const char *host) {
    g_cfg.addr    = addr;
    g_cfg.ssl_ctx = ssl_ctx;
    g_cfg.host    = host;
}

/* ------------------------------------------------------------------------- */
/* Per-connection state (lives entirely inside conn->proto_state)            */
/* ------------------------------------------------------------------------- */

typedef struct http1_state {
    transport            xport;       /* TCP/TLS transport for this conn       */
    http_parser          parser;      /* response parser                       */
    http_parser_settings settings;    /* completion-detection callbacks        */

    char   rbuf[HTTP1_RECVBUF];       /* scratch read buffer                   */
    bool   complete;                  /* set by on_message_complete            */
    bool   error;                     /* set on parse / transport failure      */
    bool   keep_alive;                /* http_should_keep_alive at completion  */
    size_t bytes;                     /* total response bytes seen             */
} http1_state;

/* ------------------------------------------------------------------------- */
/* http_parser callbacks (adapted from wrk.c)                                */
/* ------------------------------------------------------------------------- */

static int on_message_complete(http_parser *parser) {
    http1_state *s = parser->data;
    s->complete   = true;
    s->keep_alive = http_should_keep_alive(parser) != 0;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* vtable: connect                                                           */
/* ------------------------------------------------------------------------- */

static int http1_connect(connection *c) {
    http1_state *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    transport_init(&s->xport, g_cfg.addr, g_cfg.ssl_ctx, g_cfg.host);

    int fd = -1;
    if (transport_connect(&s->xport, &fd) != TRANSPORT_OK) {
        free(s);
        return -1;
    }

    http_parser_init(&s->parser, HTTP_RESPONSE);
    s->parser.data = s;

    memset(&s->settings, 0, sizeof(s->settings));
    s->settings.on_message_complete = on_message_complete;

    s->complete = false;
    s->error    = false;
    s->bytes    = 0;

    c->fd          = fd;
    c->proto_state = s;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* vtable: write                                                             */
/* ------------------------------------------------------------------------- */

static int http1_write(connection *c, const char *buf, size_t len) {
    http1_state *s = c->proto_state;
    if (!s) return -1;

    /* Complete the TLS handshake before the first request bytes go out. */
    switch (transport_handshake(&s->xport)) {
        case TRANSPORT_OK:    break;
        case TRANSPORT_RETRY: return 0;   /* not ready yet; retry on next event */
        default:              return -1;
    }

    /* A new request starts: reset the response parser. */
    if (s->complete || s->error) {
        http_parser_init(&s->parser, HTTP_RESPONSE);
        s->parser.data = s;
        s->complete = false;
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

/* ------------------------------------------------------------------------- */
/* vtable: readable                                                          */
/* ------------------------------------------------------------------------- */

static proto_status http1_readable(connection *c) {
    http1_state *s = c->proto_state;
    if (!s) return PROTO_ERROR;

    /* If the TLS handshake is still in flight, drive it on the read event. */
    switch (transport_handshake(&s->xport)) {
        case TRANSPORT_OK:    break;
        case TRANSPORT_RETRY: return PROTO_PENDING;
        default:              return PROTO_ERROR;
    }

    size_t n = 0;
    transport_status rs;

    do {
        rs = transport_read(&s->xport, s->rbuf, sizeof(s->rbuf), &n);
        switch (rs) {
            case TRANSPORT_OK:    break;
            case TRANSPORT_RETRY: return PROTO_PENDING;
            case TRANSPORT_EOF:   n = 0; break;
            default:              return PROTO_ERROR;
        }

        if (n > 0) {
            size_t parsed = http_parser_execute(&s->parser, &s->settings,
                                                s->rbuf, n);
            if (parsed != n || s->parser.http_errno != 0) {
                s->error = true;
                return PROTO_ERROR;
            }
            s->bytes += n;
        }

        if (s->complete) {
            /* A full response arrived. Classify (ADR 0003-B):
             *   - non-2xx          -> PROTO_DONE_STATUS_ERR (status takes
             *                         priority; the orchestrator counts it)
             *   - 2xx, peer closing -> PROTO_DONE_CLOSE (server sent
             *                         Connection: close or HTTP/1.0 w/o
             *                         keep-alive; orchestrator reconnects
             *                         cleanly without a read error)
             *   - 2xx, keep-alive   -> PROTO_DONE */
            int sc = s->parser.status_code;
            proto_status result;
            if (sc / 100 != 2)        result = PROTO_DONE_STATUS_ERR;
            else if (!s->keep_alive)  result = PROTO_DONE_CLOSE;
            else                      result = PROTO_DONE;

            /* Consume the completion so a *subsequent* readable event on this
             * connection does not re-report the same response (the t036
             * phantom-completion flood). On a Connection: close server an EOF
             * readable event fires immediately after the response; without
             * consuming, s->complete stays true and the level-triggered EOF
             * re-enters here every loop iteration. Reinitialise the parser for
             * the next response on a kept-alive connection. For PROTO_DONE_CLOSE
             * the orchestrator reconnects, so the reinit is harmless. */
            s->complete = false;
            http_parser_init(&s->parser, HTTP_RESPONSE);
            s->parser.data = s;
            /* Surface this response's wire size to the orchestrator before
             * resetting our per-response accumulator (t042). Set for every
             * PROTO_DONE* result so status-error and close completions are also
             * counted toward Transfer/sec, matching phase-0 wrk.c. */
            c->bytes = s->bytes;
            s->bytes = 0;
            return result;
        }

        if (rs == TRANSPORT_EOF) {
            /* Peer closed before a complete message — treat as failure unless a
             * response was already completed (handled above). */
            return PROTO_ERROR;
        }
    } while (n == sizeof(s->rbuf) && transport_pending(&s->xport) > 0);

    return PROTO_PENDING;
}

/* ------------------------------------------------------------------------- */
/* vtable: close                                                             */
/* ------------------------------------------------------------------------- */

static void http1_close(connection *c) {
    http1_state *s = c->proto_state;
    if (s) {
        transport_close(&s->xport);
        free(s);
        c->proto_state = NULL;
    }
    c->fd = -1;
}

/* ------------------------------------------------------------------------- */
/* vtable accessor                                                           */
/* ------------------------------------------------------------------------- */

static protocol http1 = {
    .name     = "http/1.1",
    .connect  = http1_connect,
    .write    = http1_write,
    .readable = http1_readable,
    .close    = http1_close,
};

protocol *http1_protocol(void) {
    return &http1;
}
