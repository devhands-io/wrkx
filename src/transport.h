#ifndef TRANSPORT_H
#define TRANSPORT_H

/*
 * Transport layer (ADR 0001, Phase 1, Protocol Engine, step P1-3).
 *
 * Protocol-independent TCP / TLS transport. It owns the non-blocking connect,
 * the (optional) TLS handshake and the byte-level read/write primitives. It
 * knows nothing about HTTP, rate control or scripting — a protocol
 * implementation (proto/<name>.c) drives it.
 *
 * Migrated from wrk.c (connect_socket, reconnect_socket) and the net.c / ssl.c
 * read/write paths.
 *
 * The frozen `struct connection` (proto.h) carries only fd / proto_state /
 * script_state, so the connect target (resolved address, SNI host, TLS context)
 * cannot travel through it. Each protocol therefore holds a `transport`
 * instance inside its own proto_state and configures it once before connecting.
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

/* Initialise transport config. Does not open a socket. host may be NULL for
 * plain TCP; ssl_ctx NULL selects plain TCP. */
void transport_init(transport *t, struct addrinfo *addr, SSL_CTX *ssl_ctx,
                    const char *host);

/* Open a non-blocking socket and start connecting. On success *fd_out receives
 * the socket and the function returns TRANSPORT_OK; the socket may still be
 * mid-connect (the event loop will report writable when ready). For TLS the
 * handshake is deferred to the first transport_handshake() call.
 * Returns TRANSPORT_ERROR on failure. */
transport_status transport_connect(transport *t, int *fd_out);

/* Drive the TLS handshake (no-op for plain TCP, returns TRANSPORT_OK). Call on
 * the first writable event. TRANSPORT_RETRY means call again on the next
 * readable/writable event. */
transport_status transport_handshake(transport *t);

/* Read up to len bytes into buf. *n receives the byte count on TRANSPORT_OK. */
transport_status transport_read(transport *t, void *buf, size_t len, size_t *n);

/* Write up to len bytes from buf. *n receives the byte count on TRANSPORT_OK. */
transport_status transport_write(transport *t, const void *buf, size_t len,
                                 size_t *n);

/* Bytes buffered and immediately readable without blocking (TLS/socket). */
size_t transport_pending(transport *t);

/* Close and release the live socket + TLS object. Safe to call repeatedly.
 * Configuration (addr/ssl_ctx/host) is preserved so the transport can be
 * reconnected. */
void transport_close(transport *t);

#endif /* TRANSPORT_H */
