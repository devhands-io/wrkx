#ifndef MC_CODEC_H
#define MC_CODEC_H

/*
 * mc_codec.h — memcached text-protocol encode/decode helpers.
 *
 * Private to extensions/memcached/; nothing outside the extension may
 * include this header.
 *
 * Encoders write complete command lines (and data blocks for set) into a
 * caller-supplied buffer and return the number of bytes written, or -1 if
 * the buffer is too small.  They never null-terminate the output; the return
 * value is the exact byte count.
 *
 * The parser (mc_parse_reply) reads one complete server reply from a buffer.
 * All string pointers in mc_reply point directly into the caller's buffer —
 * no heap allocation.  The buffer must remain valid while the reply is in use.
 */

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Reply types
 * ---------------------------------------------------------------------- */

typedef enum {
    MC_REPLY_STORED      = 0, /* SET succeeded                            */
    MC_REPLY_NOT_STORED,      /* SET condition not met                    */
    MC_REPLY_DELETED,         /* DELETE succeeded                         */
    MC_REPLY_NOT_FOUND,       /* DELETE / INCR / DECR key absent          */
    MC_REPLY_VALUE,           /* GET hit; key/flags/data/datalen valid     */
    MC_REPLY_END,             /* GET miss (empty END\r\n)                  */
    MC_REPLY_COUNTER,         /* INCR / DECR result; counter valid         */
    MC_REPLY_CLIENT_ERR,      /* CLIENT_ERROR <msg>; errmsg/errlen valid   */
    MC_REPLY_SERVER_ERR,      /* SERVER_ERROR <msg> or bare ERROR          */
} mc_reply_type;

typedef struct {
    mc_reply_type  type;

    /* MC_REPLY_VALUE */
    const char    *key;
    size_t         keylen;
    uint32_t       flags;
    const char    *data;
    size_t         datalen;

    /* MC_REPLY_COUNTER */
    uint64_t       counter;

    /* MC_REPLY_CLIENT_ERR / MC_REPLY_SERVER_ERR */
    const char    *errmsg;
    size_t         errlen;
} mc_reply;

/* -------------------------------------------------------------------------
 * Parser status
 * ---------------------------------------------------------------------- */

typedef enum {
    MC_STATUS_PENDING = 0, /* incomplete — need more data                 */
    MC_STATUS_DONE,        /* reply fully parsed; *consumed bytes used     */
    MC_STATUS_ERROR,       /* malformed input                             */
} mc_status;

/* -------------------------------------------------------------------------
 * Command encoders
 *
 * All write into buf[0..cap-1] and return bytes written, or -1 if the
 * buffer is too small.  No null terminator is appended.
 * ---------------------------------------------------------------------- */

int mc_encode_get(char *buf, size_t cap,
                  const char *key, size_t keylen);

int mc_encode_set(char *buf, size_t cap,
                  const char *key, size_t keylen,
                  const char *val, size_t vallen,
                  uint32_t flags, uint32_t exptime);

int mc_encode_delete(char *buf, size_t cap,
                     const char *key, size_t keylen);

int mc_encode_incr(char *buf, size_t cap,
                   const char *key, size_t keylen, uint64_t delta);

int mc_encode_decr(char *buf, size_t cap,
                   const char *key, size_t keylen, uint64_t delta);

/* -------------------------------------------------------------------------
 * Reply parser
 *
 * Parse one complete server reply from [buf, buf+len).
 *
 *   MC_STATUS_DONE    — *consumed set to bytes used; fields in *out point
 *                       into buf (zero-copy).
 *   MC_STATUS_PENDING — need more data; call again with a larger buffer.
 *   MC_STATUS_ERROR   — malformed input; discard the connection.
 *
 * For GET responses the parser consumes the entire VALUE…END (or bare END)
 * sequence.  out->key and out->data point into buf and are valid only while
 * buf is intact.  For multi-value responses only the first VALUE's fields are
 * returned; the consumed count still skips the complete sequence.
 * ---------------------------------------------------------------------- */

mc_status mc_parse_reply(const char *buf, size_t len,
                         mc_reply *out, size_t *consumed);

#endif /* MC_CODEC_H */
