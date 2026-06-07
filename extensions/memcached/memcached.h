#ifndef MEMCACHED_H
#define MEMCACHED_H

/*
 * memcached protocol extension — internal header.
 * Private to extensions/memcached/; nothing outside may include this.
 */

#include "wrkx_extension.h"   /* protocol, proto_status, connection */

/* Returns a pointer to the static protocol vtable. */
protocol *memcached_protocol(void);

#endif /* MEMCACHED_H */

