#ifndef MYSQL_H
#define MYSQL_H

/*
 * MySQL protocol extension — internal header.
 * Private to extensions/mysql/; nothing outside may include this.
 *
 * ADR 0005, Phase 6 (P6-4).
 */

#include "wrkx_extension.h"

#include <netdb.h>

/* One-time process-wide configuration (called from configure callback). */
void mysql_configure(struct addrinfo *addr,
                     const char *host,
                     const char *user, const char *password,
                     const char *dbname);

/* Returns a pointer to the static protocol vtable. */
protocol *mysql_protocol(void);

#endif /* MYSQL_H */
