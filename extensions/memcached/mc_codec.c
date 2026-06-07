/*
 * mc_codec.c — memcached text-protocol encode/decode implementation.
 *
 * ADR 0005, Phase 4, t059.  No networking, no wrkx engine headers.
 */

#include "mc_codec.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Return pointer to first '\r' of a "\r\n" pair, or NULL if not found. */
static const char *find_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return buf + i;
    }
    return NULL;
}

/*
 * Parse the content of a VALUE header line (without the trailing \r\n).
 * Format: "VALUE <key> <flags> <bytes>[<space><caseid>]"
 *
 * Populates *key / *keylen / *flags / *bytes.
 * Returns 0 on success, -1 on parse error.
 */
static int parse_value_hdr(const char *line, size_t len,
                            const char **key,    size_t   *keylen,
                            uint32_t   *flags,   size_t   *bytes) {
    /* Minimum: "VALUE x 0 0" = 11 chars */
    if (len < 11) return -1;

    const char *p   = line + 6; /* skip "VALUE " */
    const char *end = line + len;

    /* key — everything up to next space */
    *key = p;
    while (p < end && *p != ' ') p++;
    if (p >= end) return -1;
    *keylen = (size_t)(p++ - *key); /* advance past space */
    if (*keylen == 0) return -1;

    /* flags — decimal uint32 */
    *flags = 0;
    while (p < end && *p != ' ') {
        if (*p < '0' || *p > '9') return -1;
        *flags = *flags * 10u + (uint32_t)(*p++ - '0');
    }
    if (p >= end) return -1;
    p++; /* advance past space */

    /* bytes — decimal size_t (ignore optional CAS id after a further space) */
    *bytes = 0;
    while (p < end && *p != ' ') {
        if (*p < '0' || *p > '9') return -1;
        *bytes = *bytes * 10 + (size_t)(*p++ - '0');
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * parse_get_response — consume a VALUE…END (or bare END) sequence
 * ---------------------------------------------------------------------- */

static mc_status parse_get_response(const char *buf, size_t len,
                                    mc_reply *out, size_t *consumed) {
    size_t pos       = 0;
    int    got_value = 0;

    while (pos < len) {
        const char *crlf = find_crlf(buf + pos, len - pos);
        if (!crlf) return MC_STATUS_PENDING;

        size_t line_len = (size_t)(crlf - (buf + pos));

        /* END terminates the get response */
        if (line_len == 3 && memcmp(buf + pos, "END", 3) == 0) {
            if (!got_value)
                out->type = MC_REPLY_END;
            *consumed = pos + 5; /* "END\r\n" */
            return MC_STATUS_DONE;
        }

        /* VALUE <key> <flags> <bytes> */
        if (line_len >= 11 && memcmp(buf + pos, "VALUE ", 6) == 0) {
            const char *key;
            size_t      keylen, bytes;
            uint32_t    flags;

            if (parse_value_hdr(buf + pos, line_len,
                                &key, &keylen, &flags, &bytes) < 0)
                return MC_STATUS_ERROR;

            size_t data_off = pos + line_len + 2; /* byte after the header \r\n */
            if (len < data_off + bytes + 2)
                return MC_STATUS_PENDING;          /* data + trailing \r\n */

            if (buf[data_off + bytes]     != '\r' ||
                buf[data_off + bytes + 1] != '\n')
                return MC_STATUS_ERROR;

            /* Capture only the first VALUE block's fields */
            if (!got_value) {
                out->type    = MC_REPLY_VALUE;
                out->key     = key;
                out->keylen  = keylen;
                out->flags   = flags;
                out->data    = buf + data_off;
                out->datalen = bytes;
                got_value    = 1;
            }

            pos = data_off + bytes + 2; /* advance past data + \r\n */
            continue;
        }

        return MC_STATUS_ERROR; /* unexpected line inside a get response */
    }

    return MC_STATUS_PENDING;
}

/* -------------------------------------------------------------------------
 * Command encoders
 * ---------------------------------------------------------------------- */

int mc_encode_get(char *buf, size_t cap,
                  const char *key, size_t keylen) {
    int n = snprintf(buf, cap, "get %.*s\r\n", (int)keylen, key);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int mc_encode_set(char *buf, size_t cap,
                  const char *key, size_t keylen,
                  const char *val, size_t vallen,
                  uint32_t flags, uint32_t exptime) {
    int hdr = snprintf(buf, cap, "set %.*s %u %u %zu\r\n",
                       (int)keylen, key, flags, exptime, vallen);
    if (hdr <= 0 || (size_t)hdr >= cap) return -1;
    if ((size_t)hdr + vallen + 2 > cap)  return -1;
    memcpy(buf + hdr, val, vallen);
    buf[hdr + vallen]     = '\r';
    buf[hdr + vallen + 1] = '\n';
    return hdr + (int)vallen + 2;
}

int mc_encode_delete(char *buf, size_t cap,
                     const char *key, size_t keylen) {
    int n = snprintf(buf, cap, "delete %.*s\r\n", (int)keylen, key);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int mc_encode_incr(char *buf, size_t cap,
                   const char *key, size_t keylen, uint64_t delta) {
    int n = snprintf(buf, cap, "incr %.*s %" PRIu64 "\r\n",
                     (int)keylen, key, delta);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int mc_encode_decr(char *buf, size_t cap,
                   const char *key, size_t keylen, uint64_t delta) {
    int n = snprintf(buf, cap, "decr %.*s %" PRIu64 "\r\n",
                     (int)keylen, key, delta);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/* -------------------------------------------------------------------------
 * Reply parser
 * ---------------------------------------------------------------------- */

mc_status mc_parse_reply(const char *buf, size_t len,
                         mc_reply *out, size_t *consumed) {
    memset(out, 0, sizeof(*out));

    const char *crlf = find_crlf(buf, len);
    if (!crlf) return MC_STATUS_PENDING;

    size_t line_len = (size_t)(crlf - buf);

    /* ---- GET responses: VALUE…END or bare END on miss ----------------- */
    if ((line_len >= 11 && memcmp(buf, "VALUE ", 6) == 0) ||
        (line_len ==  3 && memcmp(buf, "END",   3) == 0)) {
        return parse_get_response(buf, len, out, consumed);
    }

    /* ---- Storage / deletion replies ----------------------------------- */
    if (line_len == 6 && memcmp(buf, "STORED", 6) == 0) {
        out->type = MC_REPLY_STORED;
        *consumed = line_len + 2;
        return MC_STATUS_DONE;
    }
    if (line_len == 10 && memcmp(buf, "NOT_STORED", 10) == 0) {
        out->type = MC_REPLY_NOT_STORED;
        *consumed = line_len + 2;
        return MC_STATUS_DONE;
    }
    if (line_len == 7 && memcmp(buf, "DELETED", 7) == 0) {
        out->type = MC_REPLY_DELETED;
        *consumed = line_len + 2;
        return MC_STATUS_DONE;
    }
    if (line_len == 9 && memcmp(buf, "NOT_FOUND", 9) == 0) {
        out->type = MC_REPLY_NOT_FOUND;
        *consumed = line_len + 2;
        return MC_STATUS_DONE;
    }

    /* ---- Error replies ------------------------------------------------ */
    /* Bare ERROR (unrecognised command) */
    if (line_len == 5 && memcmp(buf, "ERROR", 5) == 0) {
        out->type   = MC_REPLY_SERVER_ERR;
        out->errmsg = buf + line_len;
        out->errlen = 0;
        *consumed   = line_len + 2;
        return MC_STATUS_DONE;
    }
    if (line_len >= 12 && memcmp(buf, "CLIENT_ERROR", 12) == 0) {
        out->type   = MC_REPLY_CLIENT_ERR;
        out->errmsg = (line_len > 13 && buf[12] == ' ') ? buf + 13
                                                         : buf + line_len;
        out->errlen = (line_len > 13 && buf[12] == ' ') ? line_len - 13 : 0;
        *consumed   = line_len + 2;
        return MC_STATUS_DONE;
    }
    if (line_len >= 12 && memcmp(buf, "SERVER_ERROR", 12) == 0) {
        out->type   = MC_REPLY_SERVER_ERR;
        out->errmsg = (line_len > 13 && buf[12] == ' ') ? buf + 13
                                                         : buf + line_len;
        out->errlen = (line_len > 13 && buf[12] == ' ') ? line_len - 13 : 0;
        *consumed   = line_len + 2;
        return MC_STATUS_DONE;
    }

    /* ---- Counter reply (INCR / DECR result) — one or more digits ------ */
    if (line_len > 0 && buf[0] >= '0' && buf[0] <= '9') {
        uint64_t v = 0;
        for (size_t i = 0; i < line_len; i++) {
            if (buf[i] < '0' || buf[i] > '9') return MC_STATUS_ERROR;
            v = v * 10 + (uint64_t)(buf[i] - '0');
        }
        out->type    = MC_REPLY_COUNTER;
        out->counter = v;
        *consumed    = line_len + 2;
        return MC_STATUS_DONE;
    }

    return MC_STATUS_ERROR;
}
