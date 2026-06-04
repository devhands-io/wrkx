#ifndef PROTO_HTTP1_H
#define PROTO_HTTP1_H

/*
 * HTTP/1.1 protocol implementation (ADR 0001, Phase 1, Protocol Engine, P1-3).
 *
 * Implements the `protocol` vtable from proto.h for HTTP/1.1: TCP/TLS connect,
 * request send, response parse + completion detection, close. All per-connection
 * state (transport, http_parser, read buffer, completion flags) lives inside
 * conn->proto_state — the frozen `struct connection` gains no fields.
 *
 * Invariant 2: this header and proto/http1.c must not include any scripting
 * header (lua.h, quickjs.h, ...).
 */

#include <netdb.h>
#include <openssl/ssl.h>

#include "proto/proto.h"

/*
 * Returns the singleton HTTP/1.1 protocol vtable.
 *
 * The frozen `protocol.connect` takes only a `connection *`, so the connect
 * target (resolved address, TLS context, SNI host) cannot be passed per-call.
 * http1_configure() supplies that target once for the process before any
 * connection is opened. See the "frozen contract" note in proto/http1.c.
 */
protocol *http1_protocol(void);

/*
 * Configure the connect target shared by all HTTP/1.1 connections.
 *   addr    - resolved getaddrinfo() result for the host:port (borrowed)
 *   ssl_ctx - TLS context, or NULL for plain HTTP (borrowed)
 *   host    - SNI / Host name (borrowed)
 * Must be called before the first connect(). Pointers are borrowed and must
 * outlive every connection.
 */
void http1_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx, const char *host);

#endif /* PROTO_HTTP1_H */
