#ifndef REDIS_H
#define REDIS_H

/*
 * Redis protocol extension (ADR 0005, Phase 2, P2-1).
 *
 * Implements the `protocol` vtable for Redis over RESP. One command per
 * request (no pipelining — deferred to t052). AUTH and SELECT are handled
 * synchronously inside connect(), keeping the orchestrator's event loop
 * unaware of the connection handshake.
 *
 * Invariant 2: no scripting header is included here.
 */

#include "proto/proto.h"

#include <netdb.h>
#include <openssl/ssl.h>

/*
 * One-time process-wide configuration (mirrors http1_configure).
 * Must be called before orchestrator_create / any redis_connect().
 *
 *   addr     — resolved connect target (borrowed; must outlive connections)
 *   ssl_ctx  — NULL for plain TCP (borrowed)
 *   host     — SNI host name; may be NULL for plain TCP (borrowed)
 *   password — NULL means no AUTH handshake
 *   db       — 0 means no SELECT (databases 1-15 trigger SELECT <db>)
 */
void redis_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                     const char *host, const char *password, int db);

/* Returns a pointer to the static protocol vtable. */
protocol *redis_protocol(void);

#endif /* REDIS_H */
