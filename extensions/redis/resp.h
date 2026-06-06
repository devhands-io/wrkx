#ifndef RESP_H
#define RESP_H

/*
 * RESP (REdis Serialization Protocol) codec.
 * Internal to the redis extension; not exported to the host.
 */

#include <stddef.h>

int  resp_encode(char *buf, size_t cap, int argc,
                 const char * const *argv, const size_t *arglens);
int  resp_parse(const char *buf, size_t len, size_t *out_bytes);

#endif /* RESP_H */
