#ifndef POSTGRES_H
#define POSTGRES_H

/*
 * PostgreSQL protocol extension — internal header.
 * Private to extensions/postgres/; nothing outside may include this.
 *
 * ADR 0005, Phase 6 (P6-1 + P6-2).
 */

#include "wrkx_extension.h"

#include <netdb.h>
#include <openssl/ssl.h>

/* One-time process-wide configuration (called from configure callback). */
void postgres_configure(struct addrinfo *addr, SSL_CTX *ssl_ctx,
                        const char *host,
                        const char *user, const char *password,
                        const char *dbname);

/* Returns a pointer to the static protocol vtable. */
protocol *postgres_protocol(void);

/* Exposed for unit tests; counts Q and Sync tags in a write buffer. */
int32_t count_rfq_expected(const char *buf, size_t len);

#endif /* POSTGRES_H */
