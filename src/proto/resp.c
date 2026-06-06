/*
 * RESP (REdis Serialization Protocol) codec (ADR 0005, Phase 2, P2-1).
 *
 * Encode: format a Redis command as a RESP bulk-string array.
 * Decode: detect when one complete RESP response has arrived in a buffer.
 */

#include "proto/resp.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Encode
 * ---------------------------------------------------------------------- */

int resp_encode(char *buf, size_t cap, int argc,
                const char * const *argv, const size_t *arglens) {
    if (!buf || cap == 0 || argc <= 0 || !argv || !arglens) return -1;

    size_t pos = 0;

#define APPEND(fmt, ...) \
    do { \
        int _n = snprintf(buf + pos, cap - pos, fmt, ##__VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= cap - pos) return -1; \
        pos += (size_t)_n; \
    } while (0)

    APPEND("*%d\r\n", argc);

    for (int i = 0; i < argc; i++) {
        APPEND("$%zu\r\n", arglens[i]);
        if (arglens[i] > 0) {
            if (cap - pos < arglens[i] + 2) return -1;
            memcpy(buf + pos, argv[i], arglens[i]);
            pos += arglens[i];
            buf[pos++] = '\r';
            buf[pos++] = '\n';
        } else {
            APPEND("\r\n");
        }
    }

#undef APPEND

    return (int)pos;
}

/* -------------------------------------------------------------------------
 * Decode helpers
 * ---------------------------------------------------------------------- */

/*
 * Find the next \r\n in buf[0..len-1].
 * Returns pointer to the \r, or NULL if not found.
 */
static const char *find_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return buf + i;
    }
    return NULL;
}

/*
 * Parse a decimal integer (possibly negative) from buf, up to the \r\n.
 * Returns 1 on success (sets *val and *consumed to bytes including \r\n),
 * 0 if not enough data, -1 on error.
 */
static int parse_integer_line(const char *buf, size_t len,
                              long long *val, size_t *consumed) {
    const char *crlf = find_crlf(buf, len);
    if (!crlf) return 0;  /* need more data */

    char tmp[32];
    size_t n = (size_t)(crlf - buf);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, buf, n);
    tmp[n] = '\0';

    char *end;
    *val = strtoll(tmp, &end, 10);
    if (end != tmp + n) return -1;  /* garbage in the number */

    *consumed = n + 2;  /* number bytes + \r\n */
    return 1;
}

/*
 * Parse one RESP value starting at buf[0..len-1].
 * Returns total bytes consumed (>0), 0 (need more), or -1 (error).
 */
static int resp_parse_one(const char *buf, size_t len);

static int resp_parse_bulk(const char *buf, size_t len) {
    /* buf[0] == '$', already consumed by caller */
    long long blen;
    size_t    hdr;
    int rc = parse_integer_line(buf + 1, len - 1, &blen, &hdr);
    if (rc <= 0) return rc;
    hdr += 1;  /* account for '$' */

    if (blen < 0) return (int)hdr;  /* nil bulk string: $-1\r\n */

    size_t total = hdr + (size_t)blen + 2;  /* header + data + \r\n */
    if (len < total) return 0;
    /* verify trailing \r\n */
    if (buf[hdr + (size_t)blen] != '\r' || buf[hdr + (size_t)blen + 1] != '\n')
        return -1;
    return (int)total;
}

static int resp_parse_array(const char *buf, size_t len) {
    /* buf[0] == '*', already consumed by caller */
    long long count;
    size_t    hdr;
    int rc = parse_integer_line(buf + 1, len - 1, &count, &hdr);
    if (rc <= 0) return rc;
    hdr += 1;  /* account for '*' */

    if (count <= 0) return (int)hdr;  /* null array or empty */

    size_t pos = hdr;
    for (long long i = 0; i < count; i++) {
        if (pos >= len) return 0;
        int elem = resp_parse_one(buf + pos, len - pos);
        if (elem <= 0) return elem;
        pos += (size_t)elem;
    }
    return (int)pos;
}

static int resp_parse_one(const char *buf, size_t len) {
    if (len == 0) return 0;

    switch (buf[0]) {
        case '+':
        case '-':
        case ':': {
            /* Simple line reply: read to \r\n */
            const char *crlf = find_crlf(buf + 1, len - 1);
            if (!crlf) return 0;
            return (int)(crlf - buf) + 2;
        }
        case '$':
            return resp_parse_bulk(buf, len);
        case '*':
            return resp_parse_array(buf, len);
        default:
            return -1;  /* unknown type byte */
    }
}

/* -------------------------------------------------------------------------
 * Public decode entry point
 * ---------------------------------------------------------------------- */

int resp_parse(const char *buf, size_t len, size_t *out_bytes) {
    int rc = resp_parse_one(buf, len);
    if (rc > 0 && out_bytes)
        *out_bytes = (size_t)rc;
    return rc;
}
