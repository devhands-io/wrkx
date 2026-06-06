#ifndef WRKX_TRANSPORT_H
#define WRKX_TRANSPORT_H

/*
 * wrkx transport layer (ADR 0001, Phase 1, Protocol Engine, step P1-3).
 *
 * Protocol-independent TCP / TLS transport. It owns the non-blocking connect,
 * the (optional) TLS handshake and the byte-level read/write primitives.
 *
 * Published as a stable public interface in include/ so that protocol
 * extensions (extensions/<name>/) may use it without depending on src/.
 * Internal code continues to use src/transport.h (a thin re-export).
 */

#include <stddef.h>
#include <stdbool.h>
#include <netdb.h>
#include <openssl/ssl.h>

typedef enum {
    TRANSPORT_OK,     /* operation completed                                  */
    TRANSPORT_ERROR,  /* fatal error; caller should reconnect / fail          */
    TRANSPORT_RETRY,  /* would block (EAGAIN / WANT_READ / WANT_WRITE)        */
    TRANSPORT_EOF     /* peer closed the connection                           */
} transport_status;

/*
 * Per-connection transport state. Embedded inside a protocol's proto_state.
 * `addr` and `ssl_ctx` are configuration (not owned by the transport); `fd`
 * and `ssl` are the live resources the transport owns and releases on close.
 */
typedef struct transport {
    struct addrinfo *addr;     /* resolved connect target (borrowed)          */
    SSL_CTX         *ssl_ctx;  /* NULL for plain TCP (borrowed)               */
    const char      *host;     /* SNI host name for TLS (borrowed)            */

    int   fd;                  /* live socket, -1 when closed                 */
    SSL  *ssl;                 /* live TLS object, NULL for plain TCP         */
    bool  handshaking;         /* true until the TLS handshake completes      */
} transport;

void transport_init(transport *t, struct addrinfo *addr, SSL_CTX *ssl_ctx,
                    const char *host);
transport_status transport_connect(transport *t, int *fd_out);
transport_status transport_handshake(transport *t);
transport_status transport_read(transport *t, void *buf, size_t len, size_t *n);
transport_status transport_write(transport *t, const void *buf, size_t len,
                                 size_t *n);
size_t transport_pending(transport *t);
void transport_close(transport *t);

#endif /* WRKX_TRANSPORT_H */
