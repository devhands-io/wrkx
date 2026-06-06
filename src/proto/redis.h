#ifndef REDIS_H
#define REDIS_H

/*
 * Redis protocol extension (ADR 0005, Phase 2).
 *
 * Implements the `protocol` vtable for Redis over RESP. AUTH and SELECT are
 * handled synchronously inside connect(). Pipelining (t052) is a protocol
 * concern: the vtable auto-detects how many commands are in the write buffer
 * and accumulates that many responses before returning PROTO_DONE.
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

/*
 * Format a Redis command as heap-allocated RESP bytes.
 * Returns the buffer (caller must free) and sets *len_out to its length.
 * Returns NULL on allocation failure or encode error.
 *
 * Used by the Lua glue module so redis_helpers.c only includes proto/redis.h,
 * never proto/resp.h directly (Invariant 4 / grep check).
 */
char *redis_make_request(int argc, const char * const *argv,
                         const size_t *arglens, size_t *len_out);

#endif /* REDIS_H */
