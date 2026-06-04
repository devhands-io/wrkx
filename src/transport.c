/*
 * Transport layer implementation (ADR 0001, Phase 1, Protocol Engine, P1-3).
 *
 * Protocol-independent TCP / TLS. Migrated from wrk.c's connect_socket plus the
 * net.c / ssl.c read/write primitives, collapsed behind a single vtable-free
 * API that a protocol implementation drives.
 *
 * Invariant 2: no scripting header is included here (this file is part of the
 * Protocol Engine layer).
 */

#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

void transport_init(transport *t, struct addrinfo *addr, SSL_CTX *ssl_ctx,
                    const char *host) {
    t->addr        = addr;
    t->ssl_ctx     = ssl_ctx;
    t->host        = host;
    t->fd          = -1;
    t->ssl         = NULL;
    t->handshaking = false;
}

transport_status transport_connect(transport *t, int *fd_out) {
    struct addrinfo *addr = t->addr;
    int fd, flags;

    if (!addr) return TRANSPORT_ERROR;

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (fd < 0) return TRANSPORT_ERROR;

    flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return TRANSPORT_ERROR;
    }

    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) {
            close(fd);
            return TRANSPORT_ERROR;
        }
    }

    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

    t->fd = fd;

    if (t->ssl_ctx) {
        t->ssl = SSL_new(t->ssl_ctx);
        if (!t->ssl) {
            close(fd);
            t->fd = -1;
            return TRANSPORT_ERROR;
        }
        SSL_set_fd(t->ssl, fd);
        if (t->host) SSL_set_tlsext_host_name(t->ssl, t->host);
        t->handshaking = true;
    }

    if (fd_out) *fd_out = fd;
    return TRANSPORT_OK;
}

transport_status transport_handshake(transport *t) {
    int r;

    if (!t->ssl) return TRANSPORT_OK;          /* plain TCP: nothing to do    */
    if (!t->handshaking) return TRANSPORT_OK;  /* already established         */

    if ((r = SSL_connect(t->ssl)) == 1) {
        t->handshaking = false;
        return TRANSPORT_OK;
    }

    switch (SSL_get_error(t->ssl, r)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return TRANSPORT_RETRY;
        default:
            return TRANSPORT_ERROR;
    }
}

transport_status transport_read(transport *t, void *buf, size_t len,
                                size_t *n) {
    if (t->ssl) {
        int r = SSL_read(t->ssl, buf, (int)len);
        if (r <= 0) {
            switch (SSL_get_error(t->ssl, r)) {
                case SSL_ERROR_WANT_READ:
                case SSL_ERROR_WANT_WRITE: return TRANSPORT_RETRY;
                case SSL_ERROR_ZERO_RETURN: return TRANSPORT_EOF;
                default:                    return TRANSPORT_ERROR;
            }
        }
        *n = (size_t)r;
        return TRANSPORT_OK;
    }

    ssize_t r = read(t->fd, buf, len);
    if (r == 0) return TRANSPORT_EOF;
    if (r < 0) {
        switch (errno) {
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EINTR:  return TRANSPORT_RETRY;
            default:     return TRANSPORT_ERROR;
        }
    }
    *n = (size_t)r;
    return TRANSPORT_OK;
}

transport_status transport_write(transport *t, const void *buf, size_t len,
                                 size_t *n) {
    if (t->ssl) {
        int r = SSL_write(t->ssl, buf, (int)len);
        if (r <= 0) {
            switch (SSL_get_error(t->ssl, r)) {
                case SSL_ERROR_WANT_READ:
                case SSL_ERROR_WANT_WRITE: return TRANSPORT_RETRY;
                default:                   return TRANSPORT_ERROR;
            }
        }
        *n = (size_t)r;
        return TRANSPORT_OK;
    }

    ssize_t r = write(t->fd, buf, len);
    if (r < 0) {
        switch (errno) {
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EINTR:  return TRANSPORT_RETRY;
            default:     return TRANSPORT_ERROR;
        }
    }
    *n = (size_t)r;
    return TRANSPORT_OK;
}

size_t transport_pending(transport *t) {
    if (t->ssl) {
        int p = SSL_pending(t->ssl);
        return p > 0 ? (size_t)p : 0;
    }
    int n = 0;
    if (ioctl(t->fd, FIONREAD, &n) == -1) return 0;
    return n > 0 ? (size_t)n : 0;
}

void transport_close(transport *t) {
    if (t->ssl) {
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
        t->ssl = NULL;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    t->handshaking = false;
}
