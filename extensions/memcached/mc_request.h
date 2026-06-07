#ifndef MC_REQUEST_H
#define MC_REQUEST_H

/*
 * mc_request.h — memcached operation model.
 *
 * Sits between the Lua helpers (t061) and the text codec (mc_codec.h).
 * Defines typed request/response structs so upper layers never deal with
 * raw format strings.
 *
 * Memory ownership
 * ----------------
 * mc_request  — all pointer fields (key, value) are borrowed.  The caller
 *               retains ownership; the data must live until mc_request_encode
 *               returns.
 *
 * mc_response — pointer fields (value, errmsg) borrow from the receive buffer.
 *               They are invalidated as soon as the buffer is reused or freed.
 *
 * Neither struct performs heap allocation.
 *
 * Fields used per operation
 * -------------------------
 *   GET    : op, key, keylen
 *   SET    : op, key, keylen, value, vallen, flags, exptime
 *   DELETE : op, key, keylen
 *   INCR   : op, key, keylen, delta
 *   DECR   : op, key, keylen, delta
 *
 * Initialise unused fields to zero (use designated initialisers or memset).
 */

#include "mc_codec.h"  /* mc_reply_type, mc_status */
#include <stddef.h>
#include <stdint.h>

/* Maximum key length imposed by the memcached text protocol. */
#define MC_KEY_MAX 250

/* -------------------------------------------------------------------------
 * Operation enum
 * ---------------------------------------------------------------------- */

typedef enum {
    MC_OP_GET    = 0,
    MC_OP_SET,
    MC_OP_DELETE,
    MC_OP_INCR,
    MC_OP_DECR,
} mc_op;

/* -------------------------------------------------------------------------
 * Request
 * ---------------------------------------------------------------------- */

typedef struct {
    mc_op       op;
    const char *key;
    size_t      keylen;
    const char *value;    /* SET: data bytes; NULL treated as empty      */
    size_t      vallen;   /* SET: byte count (may be 0)                  */
    uint32_t    flags;    /* SET: client-managed flags                   */
    uint32_t    exptime;  /* SET: TTL in seconds, 0 = never expire       */
    uint64_t    delta;    /* INCR / DECR: amount to add / subtract       */
} mc_request;

/* -------------------------------------------------------------------------
 * Response
 * ---------------------------------------------------------------------- */

typedef struct {
    mc_reply_type  status;
    /* GET hit (status == MC_REPLY_VALUE) */
    const char    *value;
    size_t         vallen;
    uint32_t       flags;
    /* INCR / DECR result (status == MC_REPLY_COUNTER) */
    uint64_t       counter;
    /* Error message (status == MC_REPLY_{CLIENT,SERVER}_ERR) */
    const char    *errmsg;
    size_t         errlen;
} mc_response;

/* -------------------------------------------------------------------------
 * Functions
 * ---------------------------------------------------------------------- */

/*
 * Validate a request.
 * Returns 0 if valid, -1 on error:
 *   - req is NULL
 *   - key is NULL, empty, or longer than MC_KEY_MAX
 *   - key contains ASCII space, control characters (≤ 0x20), or DEL (0x7f)
 *   - SET with value == NULL and vallen > 0
 */
int mc_request_validate(const mc_request *req);

/*
 * Encode req into buf[0..cap-1].
 * Calls mc_request_validate first; returns -1 on invalid input or small buf.
 * Returns exact byte count written (no null terminator).
 */
int mc_request_encode(const mc_request *req, char *buf, size_t cap);

/*
 * Parse one complete server reply into resp.
 * Thin wrapper around mc_parse_reply; see mc_codec.h for semantics.
 */
mc_status mc_response_parse(const char *buf, size_t len,
                             mc_response *resp, size_t *consumed);

#endif /* MC_REQUEST_H */
