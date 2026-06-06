#ifndef REDIS_H
#define REDIS_H

/*
 * Redis protocol extension — internal header.
 * Private to extensions/redis/; nothing outside may include this.
 */

#include "wrkx_extension.h"   /* protocol, proto_status, connection */

#include <stddef.h>
#include <netdb.h>
#include <openssl/ssl.h>

/* One-time process-wide configuration (must be called before any connections). */
void redis_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                     const char *host, const char *password, int db);

/* Returns a pointer to the static protocol vtable. */
protocol *redis_protocol(void);

/* Format a Redis command as heap-allocated RESP bytes.
 * Caller must free the returned buffer. Returns NULL on failure. */
char *redis_make_request(int argc, const char * const *argv,
                         const size_t *arglens, size_t *len_out);

#endif /* REDIS_H */
