#ifndef MEMCACHED_H
#define MEMCACHED_H

/*
 * memcached protocol extension — internal header.
 * Private to extensions/memcached/; nothing outside may include this.
 */

#include "wrkx_extension.h"   /* protocol, proto_status, connection */
#include <netdb.h>
#include <openssl/ssl.h>

/* Store connection parameters; must be called before any connect(). */
void memcached_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                         const char *host, const char *password);

/* Returns a pointer to the static protocol vtable. */
protocol *memcached_protocol(void);

#endif /* MEMCACHED_H */

