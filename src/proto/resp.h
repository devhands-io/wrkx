#ifndef RESP_H
#define RESP_H

/*
 * RESP (REdis Serialization Protocol) codec (ADR 0005, Phase 2, P2-1).
 *
 * Encode: format a Redis command as a RESP bulk-string array.
 * Decode: detect when one complete RESP response has arrived in a buffer.
 *
 * Used by:
 *   proto/redis.c    — decode (readable path)
 *   scripting glue   — encode (t050: redis.command() helper)
 *
 * Invariant 2: no scripting header is included here.
 */

#include <stddef.h>

/*
 * Encode one Redis command as a RESP bulk-string array into buf[0..cap-1].
 * argc / argv / arglens describe the command and its arguments.
 * arglens[i] is the byte length of argv[i] (allows binary-safe values).
 * Returns the number of bytes written (>0) or -1 if cap is too small.
 * The output is NOT NUL-terminated.
 */
int resp_encode(char *buf, size_t cap, int argc,
                const char * const *argv, const size_t *arglens);

/*
 * Try to consume one complete top-level RESP value from buf[0..len-1].
 *
 * Returns:
 *   > 0  — complete; the return value is the number of bytes that form the
 *           response (caller may memmove the remainder to the front of its
 *           buffer). *out_bytes is set to the same value (wire byte count for
 *           Transfer/sec accounting).
 *     0  — incomplete; more data needed.
 *    -1  — parse error (malformed RESP).
 *
 * Handles all five RESP types:
 *   '+' simple string   '+OK\r\n'
 *   '-' error           '-ERR msg\r\n'
 *   ':' integer         ':1000\r\n'
 *   '$' bulk string     '$6\r\nfoobar\r\n'  (and '$-1\r\n' nil)
 *   '*' array           '*2\r\n...\r\n...\r\n'  (and '*-1\r\n' null array)
 *
 * Arrays are parsed iteratively. Nested arrays are not supported beyond
 * depth 1 — sufficient for all standard single-command Redis responses.
 */
int resp_parse(const char *buf, size_t len, size_t *out_bytes);

#endif /* RESP_H */
