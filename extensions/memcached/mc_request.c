/*
 * mc_request.c — memcached operation model implementation.
 *
 * ADR 0005, Phase 4, t060.  No networking, no wrkx engine headers.
 */

#include "mc_request.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Validation
 * ---------------------------------------------------------------------- */

int mc_request_validate(const mc_request *req) {
    if (!req) return -1;

    /* Key checks */
    if (!req->key || req->keylen == 0 || req->keylen > MC_KEY_MAX)
        return -1;

    for (size_t i = 0; i < req->keylen; i++) {
        unsigned char c = (unsigned char)req->key[i];
        /* memcached text protocol: no space, control chars, or DEL */
        if (c <= 0x20 || c == 0x7f) return -1;
    }

    /* SET: value pointer must be non-NULL when vallen > 0 */
    if (req->op == MC_OP_SET && req->value == NULL && req->vallen > 0)
        return -1;

    return 0;
}

/* -------------------------------------------------------------------------
 * Encoding
 * ---------------------------------------------------------------------- */

int mc_request_encode(const mc_request *req, char *buf, size_t cap) {
    if (mc_request_validate(req) < 0) return -1;

    static const char empty[] = "";

    switch (req->op) {
        case MC_OP_GET:
            return mc_encode_get(buf, cap, req->key, req->keylen);

        case MC_OP_SET:
            return mc_encode_set(buf, cap,
                                 req->key,   req->keylen,
                                 req->value ? req->value : empty,
                                 req->vallen,
                                 req->flags, req->exptime);

        case MC_OP_DELETE:
            return mc_encode_delete(buf, cap, req->key, req->keylen);

        case MC_OP_INCR:
            return mc_encode_incr(buf, cap, req->key, req->keylen,
                                  req->delta);

        case MC_OP_DECR:
            return mc_encode_decr(buf, cap, req->key, req->keylen,
                                  req->delta);

        default:
            return -1;
    }
}

/* -------------------------------------------------------------------------
 * Response parsing
 * ---------------------------------------------------------------------- */

mc_status mc_response_parse(const char *buf, size_t len,
                             mc_response *resp, size_t *consumed) {
    mc_reply  reply;
    mc_status s = mc_parse_reply(buf, len, &reply, consumed);
    if (s != MC_STATUS_DONE) return s;

    memset(resp, 0, sizeof(*resp));
    resp->status  = reply.type;
    resp->value   = reply.data;
    resp->vallen  = reply.datalen;
    resp->flags   = reply.flags;
    resp->counter = reply.counter;
    resp->errmsg  = reply.errmsg;
    resp->errlen  = reply.errlen;
    return MC_STATUS_DONE;
}
