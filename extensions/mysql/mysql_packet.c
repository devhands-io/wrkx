/*
 * mysql_packet.c — MySQL Client/Server Protocol wire codec.
 *
 * Implements packet header framing, length-encoded integers, HandshakeV10
 * parsing, HandshakeResponse encoding, COM_QUERY / COM_QUIT encoding, and
 * result-set packet parsing (column count, column def, row, OK, ERR, EOF).
 *
 * Also provides mysql_native_password and mysql_sha2_password_fast auth helpers
 * using OpenSSL EVP.
 *
 * No networking, no wrkx core headers.  Suitable for unit testing in isolation.
 *
 * ADR 0005, Phase 6 (P6-4).
 */

#include "mysql_packet.h"

#include <string.h>
#include <stdio.h>
#include <openssl/evp.h>

/* -------------------------------------------------------------------------
 * Little-endian helpers
 * ---------------------------------------------------------------------- */

static uint32_t rd_le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_le24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* -------------------------------------------------------------------------
 * Packet header
 * ---------------------------------------------------------------------- */

void mysql_write_pkt_header(uint8_t *buf, uint32_t payload_len, uint8_t seq) {
    wr_le24(buf, payload_len);
    buf[3] = seq;
}

int mysql_read_pkt_header(const uint8_t *buf, size_t avail,
                          uint32_t *out_len, uint8_t *out_seq) {
    if (avail < 4) return 0;
    *out_len = rd_le24(buf);
    *out_seq = buf[3];
    return 4;
}

/* -------------------------------------------------------------------------
 * Length-encoded integer (LEI)
 * ---------------------------------------------------------------------- */

int mysql_read_lei(const uint8_t *buf, size_t avail, uint64_t *out) {
    if (avail < 1) return 0;
    uint8_t b = buf[0];
    if (b < 251) {
        *out = b;
        return 1;
    }
    if (b == 0xfb) {
        *out = UINT64_MAX;   /* NULL sentinel */
        return 1;
    }
    if (b == 0xfc) {
        if (avail < 3) return 0;
        *out = (uint64_t)buf[1] | ((uint64_t)buf[2] << 8);
        return 3;
    }
    if (b == 0xfd) {
        if (avail < 4) return 0;
        *out = (uint64_t)buf[1] | ((uint64_t)buf[2] << 8) |
               ((uint64_t)buf[3] << 16);
        return 4;
    }
    /* 0xfe — 8-byte integer */
    if (avail < 9) return 0;
    *out = (uint64_t)buf[1] | ((uint64_t)buf[2] << 8) |
           ((uint64_t)buf[3] << 16) | ((uint64_t)buf[4] << 24) |
           ((uint64_t)buf[5] << 32) | ((uint64_t)buf[6] << 40) |
           ((uint64_t)buf[7] << 48) | ((uint64_t)buf[8] << 56);
    return 9;
}

int mysql_write_lei(uint8_t *buf, size_t cap, uint64_t val) {
    if (val < 251) {
        if (cap < 1) return 0;
        buf[0] = (uint8_t)val;
        return 1;
    }
    if (val < 0x10000) {
        if (cap < 3) return 0;
        buf[0] = 0xfc;
        buf[1] = (uint8_t)val;
        buf[2] = (uint8_t)(val >> 8);
        return 3;
    }
    if (val < 0x1000000) {
        if (cap < 4) return 0;
        buf[0] = 0xfd;
        buf[1] = (uint8_t)val;
        buf[2] = (uint8_t)(val >> 8);
        buf[3] = (uint8_t)(val >> 16);
        return 4;
    }
    if (cap < 9) return 0;
    buf[0] = 0xfe;
    buf[1] = (uint8_t)val;
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val >> 16);
    buf[4] = (uint8_t)(val >> 24);
    buf[5] = (uint8_t)(val >> 32);
    buf[6] = (uint8_t)(val >> 40);
    buf[7] = (uint8_t)(val >> 48);
    buf[8] = (uint8_t)(val >> 56);
    return 9;
}

/* -------------------------------------------------------------------------
 * Internal: consume a LEI-prefixed string into dst (truncated to dst_cap-1)
 * Returns bytes consumed from buf, or 0 if incomplete / too small.
 * ---------------------------------------------------------------------- */

static int read_lei_str(const uint8_t *buf, size_t avail,
                        char *dst, size_t dst_cap) {
    uint64_t slen;
    int hlen = mysql_read_lei(buf, avail, &slen);
    if (!hlen || slen == UINT64_MAX) return 0;
    if (avail < (size_t)hlen + (size_t)slen) return 0;
    size_t copy = (slen < dst_cap - 1) ? (size_t)slen : dst_cap - 1;
    memcpy(dst, buf + hlen, copy);
    dst[copy] = '\0';
    return hlen + (int)slen;
}

/* -------------------------------------------------------------------------
 * Packet parser
 * ---------------------------------------------------------------------- */

int mysql_parse_packet(const uint8_t *buf, size_t avail,
                       mysql_ctx ctx, mysql_parsed_pkt *out) {
    uint32_t payload_len;
    uint8_t  seq;
    if (!mysql_read_pkt_header(buf, avail, &payload_len, &seq)) return 0;

    size_t total = 4 + (size_t)payload_len;
    if (avail < total) return 0;

    const uint8_t *p    = buf + 4;          /* payload start */
    size_t         plen = payload_len;

    memset(out, 0, sizeof(*out));
    out->seq = seq;

    if (plen == 0) {
        out->type = MYSQL_PKT_UNKNOWN;
        return (int)total;
    }

    uint8_t tag = p[0];

    /* -----------------------------------------------------------------------
     * MYSQL_CTX_AUTH: called from connect() handshake loop.
     *   0x00 → OK
     *   0x01 → AUTH_MORE_DATA
     *   0xfe → AUTH_SWITCH_REQUEST (or Protocol-4.1 OK if payload == 5)
     *   0xff → ERR
     *   0x0a → HandshakeV10
     * -------------------------------------------------------------------- */
    if (ctx == MYSQL_CTX_AUTH) {
        if (tag == 0x0a) {
            /* HandshakeV10 */
            out->type = MYSQL_PKT_HANDSHAKE_V10;
            out->handshake.protocol_version = tag;
            const uint8_t *pp = p + 1;
            size_t rem = plen - 1;

            /* server_version: NUL-terminated */
            size_t vlen = strnlen((const char *)pp, rem);
            if (vlen >= rem) return -1;
            size_t copy = (vlen < 31) ? vlen : 31;
            memcpy(out->handshake.server_version, pp, copy);
            out->handshake.server_version[copy] = '\0';
            pp  += vlen + 1;
            rem -= vlen + 1;

            if (rem < 4 + 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10) return -1;

            out->handshake.connection_id = rd_le32(pp); pp += 4; rem -= 4;

            /* auth-plugin-data-part-1: 8 bytes */
            memcpy(out->handshake.auth_plugin_data, pp, 8);
            pp += 8; rem -= 8;

            /* filler: 1 byte */
            pp++; rem--;

            /* capability flags lower 2 bytes */
            uint32_t cap_lo = rd_le16(pp); pp += 2; rem -= 2;

            out->handshake.charset = *pp++; rem--;

            out->handshake.status_flags = rd_le16(pp); pp += 2; rem -= 2;

            uint32_t cap_hi = rd_le16(pp); pp += 2; rem -= 2;
            out->handshake.server_capabilities = cap_lo | (cap_hi << 16);

            /* auth_plugin_data_len (1 byte) */
            uint8_t auth_data_len = *pp++; rem--;

            /* reserved: 10 bytes */
            if (rem < 10) return -1;
            pp += 10; rem -= 10;

            /* auth-plugin-data-part-2: max(13, auth_data_len - 8) bytes */
            uint8_t part2_len = auth_data_len > 8 ? auth_data_len - 8 : 13;
            if ((size_t)part2_len > rem) part2_len = (uint8_t)rem;
            uint8_t usable = part2_len > 0 ? part2_len - 1 : 0; /* strip trailing NUL */
            if (usable > 12) usable = 12;
            memcpy(out->handshake.auth_plugin_data + 8, pp, usable);
            pp  += part2_len;
            rem -= part2_len;

            /* auth_plugin_name: NUL-terminated, optional */
            if (rem > 0) {
                size_t nlen = strnlen((const char *)pp, rem);
                size_t nc = (nlen < 31) ? nlen : 31;
                memcpy(out->handshake.auth_plugin_name, pp, nc);
                out->handshake.auth_plugin_name[nc] = '\0';
            }
            return (int)total;
        }

        if (tag == 0x00) {
            /* OK packet */
            out->type = MYSQL_PKT_OK;
            const uint8_t *pp = p + 1;
            size_t rem = plen - 1;
            uint64_t v;
            int n = mysql_read_lei(pp, rem, &v);
            if (!n) return -1;
            out->ok.affected_rows = v; pp += n; rem -= (size_t)n;
            n = mysql_read_lei(pp, rem, &v);
            if (!n) return -1;
            out->ok.last_insert_id = v; pp += n; rem -= (size_t)n;
            if (rem >= 2) {
                out->ok.status_flags = rd_le16(pp); pp += 2; rem -= 2;
            }
            if (rem >= 2) {
                out->ok.warnings = rd_le16(pp);
            }
            return (int)total;
        }

        if (tag == 0x01) {
            /* AUTH_MORE_DATA */
            out->type = MYSQL_PKT_AUTH_MORE_DATA;
            if (plen < 2) return -1;
            out->auth_more_data.marker = p[1];
            return (int)total;
        }

        if (tag == 0xfe) {
            /* Protocol-4.1 OK alias: payload == 5 bytes (0xfe + w2 + s2) */
            if (plen == 5) {
                out->type = MYSQL_PKT_OK;
                out->ok.warnings     = rd_le16(p + 1);
                out->ok.status_flags = rd_le16(p + 3);
                return (int)total;
            }
            /* AUTH_SWITCH_REQUEST: 0xfe + plugin_name\0 + auth_data */
            out->type = MYSQL_PKT_AUTH_SWITCH_REQ;
            const uint8_t *pp = p + 1;
            size_t rem = plen - 1;
            size_t nlen = strnlen((const char *)pp, rem);
            if (nlen >= rem) return -1;
            size_t nc = (nlen < 63) ? nlen : 63;
            memcpy(out->auth_switch.plugin_name, pp, nc);
            out->auth_switch.plugin_name[nc] = '\0';
            pp  += nlen + 1;
            rem -= nlen + 1;
            uint8_t dlen = (rem < 20) ? (uint8_t)rem : 20;
            memcpy(out->auth_switch.auth_data, pp, dlen);
            out->auth_switch.auth_data_len = dlen;
            return (int)total;
        }

        if (tag == 0xff) {
            /* ERR packet */
            out->type = MYSQL_PKT_ERR;
            if (plen < 3) return -1;
            out->err.error_code = rd_le16(p + 1);
            const uint8_t *pp = p + 3;
            size_t rem = plen - 3;
            /* sqlstate marker '#' + 5 chars */
            if (rem > 0 && *pp == '#') { pp++; rem--; }
            if (rem >= 5) {
                memcpy(out->err.sqlstate, pp, 5);
                out->err.sqlstate[5] = '\0';
                pp += 5; rem -= 5;
            }
            size_t mlen = (rem < 255) ? rem : 255;
            memcpy(out->err.message, pp, mlen);
            out->err.message[mlen] = '\0';
            return (int)total;
        }

        out->type = MYSQL_PKT_UNKNOWN;
        return (int)total;
    }

    /* -----------------------------------------------------------------------
     * MYSQL_CTX_GENERIC: readable() result-set preamble.
     *   0x00 → OK (no result set: INSERT/UPDATE/DELETE)
     *   0xff → ERR
     *   0xfe (len <= 9) → EOF / Protocol-4.1 OK alias
     *   otherwise → COLUMN_COUNT (LEI)
     * Note: 0x01 here means column count = 1, NOT AUTH_MORE_DATA.
     * -------------------------------------------------------------------- */
    if (ctx == MYSQL_CTX_GENERIC) {
        if (tag == 0x00) {
            out->type = MYSQL_PKT_OK;
            const uint8_t *pp = p + 1;
            size_t rem = plen - 1;
            uint64_t v;
            int n = mysql_read_lei(pp, rem, &v);
            if (n > 0) { out->ok.affected_rows = v; pp += n; rem -= (size_t)n; }
            n = mysql_read_lei(pp, rem, &v);
            if (n > 0) { out->ok.last_insert_id = v; pp += n; rem -= (size_t)n; }
            if (rem >= 2) { out->ok.status_flags = rd_le16(pp); pp += 2; rem -= 2; }
            if (rem >= 2) { out->ok.warnings = rd_le16(pp); }
            return (int)total;
        }
        if (tag == 0xff) {
            out->type = MYSQL_PKT_ERR;
            if (plen < 3) return -1;
            out->err.error_code = rd_le16(p + 1);
            const uint8_t *pp = p + 3;
            size_t rem = plen - 3;
            if (rem > 0 && *pp == '#') { pp++; rem--; }
            if (rem >= 5) {
                memcpy(out->err.sqlstate, pp, 5);
                out->err.sqlstate[5] = '\0';
                pp += 5; rem -= 5;
            }
            size_t mlen = (rem < 255) ? rem : 255;
            memcpy(out->err.message, pp, mlen);
            out->err.message[mlen] = '\0';
            return (int)total;
        }
        if (tag == 0xfe && payload_len <= 9) {
            /* EOF or Protocol-4.1 OK alias */
            out->type = MYSQL_PKT_EOF;
            if (plen >= 5) {
                out->eof.warnings     = rd_le16(p + 1);
                out->eof.status_flags = rd_le16(p + 3);
            }
            return (int)total;
        }
        /* Column count (LEI) */
        uint64_t cnt;
        int n = mysql_read_lei(p, plen, &cnt);
        if (!n || cnt == 0 || cnt == UINT64_MAX) return -1;
        out->type = MYSQL_PKT_COLUMN_COUNT;
        out->column_count.count = cnt;
        return (int)total;
    }

    /* -----------------------------------------------------------------------
     * MYSQL_CTX_COL_DEF: column definition packet (one per column).
     * Layout: catalog (LEI-str) + schema + table + org_table + name + org_name
     *         + 0x0c + charset(2) + col_length(4) + type(1) + flags(2) + dec(1) + 2-byte pad
     * -------------------------------------------------------------------- */
    if (ctx == MYSQL_CTX_COL_DEF) {
        if (tag == 0xff) {
            out->type = MYSQL_PKT_ERR;
            if (plen < 3) return -1;
            out->err.error_code = rd_le16(p + 1);
            const uint8_t *pp = p + 3; size_t rem = plen - 3;
            if (rem > 0 && *pp == '#') { pp++; rem--; }
            if (rem >= 5) { memcpy(out->err.sqlstate, pp, 5); out->err.sqlstate[5]='\0'; pp+=5; rem-=5; }
            size_t mlen = rem < 255 ? rem : 255;
            memcpy(out->err.message, pp, mlen); out->err.message[mlen]='\0';
            return (int)total;
        }
        if (tag == 0xfe && payload_len <= 9) {
            out->type = MYSQL_PKT_EOF;
            if (plen >= 5) {
                out->eof.warnings     = rd_le16(p + 1);
                out->eof.status_flags = rd_le16(p + 3);
            }
            return (int)total;
        }
        /* Parse catalog, schema, table, org_table, name, org_name (six LEI strings) */
        out->type = MYSQL_PKT_COLUMN_DEF;
        const uint8_t *pp = p;
        size_t rem = plen;
        char  tmp[64];
        /* catalog */
        int n = read_lei_str(pp, rem, tmp, sizeof(tmp)); if (!n) return -1; pp+=n; rem-=(size_t)n;
        /* schema */
        n = read_lei_str(pp, rem, out->column_def.schema, sizeof(out->column_def.schema));
        if (!n) return -1; else { pp+=n; rem-=(size_t)n; }
        /* table */
        n = read_lei_str(pp, rem, out->column_def.table, sizeof(out->column_def.table));
        if (!n) return -1; else { pp+=n; rem-=(size_t)n; }
        /* org_table */
        n = read_lei_str(pp, rem, tmp, sizeof(tmp)); if (!n) return -1; pp+=n; rem-=(size_t)n;
        /* name */
        n = read_lei_str(pp, rem, out->column_def.name, sizeof(out->column_def.name));
        if (!n) return -1; else { pp+=n; rem-=(size_t)n; }
        /* org_name */
        n = read_lei_str(pp, rem, tmp, sizeof(tmp)); if (!n) return -1; pp+=n; rem-=(size_t)n;
        /* 0x0c length marker + charset(2) + col_length(4) + type(1) */
        if (rem < 1+2+4+1) return -1;
        pp++; rem--;                         /* 0x0c */
        pp += 2; rem -= 2;                   /* charset */
        pp += 4; rem -= 4;                   /* col_length */
        out->column_def.type_oid = *pp++;
        rem--;
        return (int)total;
    }

    /* -----------------------------------------------------------------------
     * MYSQL_CTX_ROW: raw text row (any packet that isn't EOF or ERR).
     * -------------------------------------------------------------------- */
    if (ctx == MYSQL_CTX_ROW) {
        if (tag == 0xfe && payload_len <= 9) {
            out->type = MYSQL_PKT_EOF;
            if (plen >= 5) {
                out->eof.warnings     = rd_le16(p + 1);
                out->eof.status_flags = rd_le16(p + 3);
            }
            return (int)total;
        }
        if (tag == 0xff) {
            out->type = MYSQL_PKT_ERR;
            if (plen < 3) return -1;
            out->err.error_code = rd_le16(p + 1);
            const uint8_t *pp = p + 3; size_t rem = plen - 3;
            if (rem > 0 && *pp == '#') { pp++; rem--; }
            if (rem >= 5) { memcpy(out->err.sqlstate, pp, 5); out->err.sqlstate[5]='\0'; pp+=5; rem-=5; }
            size_t mlen = rem < 255 ? rem : 255;
            memcpy(out->err.message, pp, mlen); out->err.message[mlen]='\0';
            return (int)total;
        }
        out->type = MYSQL_PKT_ROW;
        return (int)total;
    }

    /* -----------------------------------------------------------------------
     * MYSQL_CTX_STMT_PREPARE: response to COM_STMT_PREPARE.
     *   0x00 → STMT_PREPARE_OK
     *   0xff → ERR
     * -------------------------------------------------------------------- */
    if (ctx == MYSQL_CTX_STMT_PREPARE) {
        if (tag == 0xff) {
            out->type = MYSQL_PKT_ERR;
            if (plen < 3) return -1;
            out->err.error_code = rd_le16(p + 1);
            const uint8_t *pp = p + 3; size_t rem = plen - 3;
            if (rem > 0 && *pp == '#') { pp++; rem--; }
            if (rem >= 5) { memcpy(out->err.sqlstate, pp, 5); out->err.sqlstate[5]='\0'; pp+=5; rem-=5; }
            size_t mlen = rem < 255 ? rem : 255;
            memcpy(out->err.message, pp, mlen); out->err.message[mlen]='\0';
            return (int)total;
        }
        if (tag == 0x00) {
            /* COM_STMT_PREPARE_OK:
               [0]=0x00  [1..4]=stmt_id  [5..6]=n_columns  [7..8]=n_params
               [9]=reserved  [10..11]=warning_count */
            if (plen < 12) return -1;
            out->type = MYSQL_PKT_STMT_PREPARE_OK;
            out->stmt_prepare_ok.stmt_id       = rd_le32(p + 1);
            out->stmt_prepare_ok.n_columns     = rd_le16(p + 5);
            out->stmt_prepare_ok.n_params      = rd_le16(p + 7);
            /* p[9] = reserved */
            out->stmt_prepare_ok.warning_count = rd_le16(p + 10);
            return (int)total;
        }
        out->type = MYSQL_PKT_UNKNOWN;
        return (int)total;
    }

    /* -----------------------------------------------------------------------
     * MYSQL_CTX_BINARY_ROW: COM_STMT_EXECUTE result row.
     *   0xfe (len <= 9) → EOF
     *   0xff → ERR
     *   anything else → BINARY_ROW
     * -------------------------------------------------------------------- */
    if (ctx == MYSQL_CTX_BINARY_ROW) {
        if (tag == 0xfe && payload_len <= 9) {
            out->type = MYSQL_PKT_EOF;
            if (plen >= 5) {
                out->eof.warnings     = rd_le16(p + 1);
                out->eof.status_flags = rd_le16(p + 3);
            }
            return (int)total;
        }
        if (tag == 0xff) {
            out->type = MYSQL_PKT_ERR;
            if (plen < 3) return -1;
            out->err.error_code = rd_le16(p + 1);
            const uint8_t *pp = p + 3; size_t rem = plen - 3;
            if (rem > 0 && *pp == '#') { pp++; rem--; }
            if (rem >= 5) { memcpy(out->err.sqlstate, pp, 5); out->err.sqlstate[5]='\0'; pp+=5; rem-=5; }
            size_t mlen = rem < 255 ? rem : 255;
            memcpy(out->err.message, pp, mlen); out->err.message[mlen]='\0';
            return (int)total;
        }
        out->type = MYSQL_PKT_BINARY_ROW;
        return (int)total;
    }

    out->type = MYSQL_PKT_UNKNOWN;
    return (int)total;
}

/* -------------------------------------------------------------------------
 * Encoder: HandshakeResponse (seq=1)
 * ---------------------------------------------------------------------- */

int mysql_encode_handshake_response(uint8_t *buf, size_t cap,
                                    const char *user, const char *db,
                                    const uint8_t *auth_resp,
                                    uint8_t auth_resp_len,
                                    const char *auth_plugin_name,
                                    uint32_t client_flags) {
    size_t user_len   = user             ? strlen(user)             : 0;
    size_t db_len     = db               ? strlen(db)               : 0;
    size_t plugin_len = auth_plugin_name ? strlen(auth_plugin_name) : 0;

    /*
     * Payload layout:
     *   capability(4) + max_pkt(4) + charset(1) + filler(23)
     *   + user\0
     *   + LEI(auth_resp_len) + auth_resp
     *   + db\0
     *   + plugin_name\0
     */
    size_t payload = 4 + 4 + 1 + 23
                   + user_len + 1
                   + 1 + auth_resp_len       /* LEI(len) + data — len < 251 always */
                   + db_len + 1
                   + plugin_len + 1;

    size_t total = 4 + payload;
    if (cap < total) return 0;

    mysql_write_pkt_header(buf, (uint32_t)payload, 1 /* seq=1 */);
    uint8_t *p = buf + 4;

    wr_le32(p, client_flags); p += 4;
    wr_le32(p, 0x01000000);   p += 4;   /* max packet size = 16 MiB */
    *p++ = 0x21;                         /* charset: utf8mb4 collation 33 */
    memset(p, 0, 23); p += 23;           /* reserved filler */

    memcpy(p, user, user_len); p += user_len; *p++ = '\0';

    *p++ = auth_resp_len;
    memcpy(p, auth_resp, auth_resp_len); p += auth_resp_len;

    memcpy(p, db, db_len); p += db_len; *p++ = '\0';

    memcpy(p, auth_plugin_name, plugin_len); p += plugin_len; *p++ = '\0';

    return (int)total;
}

/* -------------------------------------------------------------------------
 * Encoder: COM_QUERY (seq=0)
 * ---------------------------------------------------------------------- */

int mysql_encode_com_query(uint8_t *buf, size_t cap,
                           const char *sql, size_t sql_len) {
    size_t payload = 1 + sql_len;   /* 0x03 + sql bytes */
    size_t total   = 4 + payload;
    if (cap < total || payload > 0xFFFFFF) return 0;

    mysql_write_pkt_header(buf, (uint32_t)payload, 0 /* seq=0 */);
    buf[4] = 0x03;   /* COM_QUERY */
    memcpy(buf + 5, sql, sql_len);
    return (int)total;
}

/* -------------------------------------------------------------------------
 * Encoder: COM_QUIT (seq=0)
 * ---------------------------------------------------------------------- */

int mysql_encode_com_quit(uint8_t *buf, size_t cap) {
    if (cap < 5) return 0;
    mysql_write_pkt_header(buf, 1, 0);
    buf[4] = 0x01;   /* COM_QUIT */
    return 5;
}

/* -------------------------------------------------------------------------
 * Auth helpers
 * ---------------------------------------------------------------------- */

/* Compute SHA1 of (data, len) into out[20]. */
static void sha1_buf(const uint8_t *data, size_t len, uint8_t out[20]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int olen = 20;
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);
}

/* Compute SHA256 of (data, len) into out[32]. */
static void sha256_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int olen = 32;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);
}

void mysql_native_password(const char *password,
                           const uint8_t challenge[20],
                           uint8_t out[20]) {
    if (!password || !*password) {
        memset(out, 0, 20);
        return;
    }
    size_t pwlen = strlen(password);

    /* stage1 = SHA1(password) */
    uint8_t stage1[20];
    sha1_buf((const uint8_t *)password, pwlen, stage1);

    /* stage2 = SHA1(stage1) */
    uint8_t stage2[20];
    sha1_buf(stage1, 20, stage2);

    /* token = SHA1(challenge + stage2) */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int olen = 20;
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, challenge, 20);
    EVP_DigestUpdate(ctx, stage2, 20);
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);

    /* out = stage1 XOR token */
    for (int i = 0; i < 20; i++)
        out[i] ^= stage1[i];
}

void mysql_sha2_password_fast(const char *password,
                              const uint8_t challenge[20],
                              uint8_t out[32]) {
    if (!password || !*password) {
        memset(out, 0, 32);
        return;
    }
    size_t pwlen = strlen(password);

    /* stage1 = SHA256(password) */
    uint8_t stage1[32];
    sha256_buf((const uint8_t *)password, pwlen, stage1);

    /* stage2 = SHA256(stage1) */
    uint8_t stage2[32];
    sha256_buf(stage1, 32, stage2);

    /* token = SHA256(stage2 + challenge) */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int olen = 32;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, stage2, 32);
    EVP_DigestUpdate(ctx, challenge, 20);
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);

    /* out = stage1 XOR token */
    for (int i = 0; i < 32; i++)
        out[i] ^= stage1[i];
}

/* -------------------------------------------------------------------------
 * Encoder: COM_STMT_PREPARE (seq=0, cmd=0x16)
 * ---------------------------------------------------------------------- */

int mysql_encode_com_stmt_prepare(uint8_t *buf, size_t cap,
                                  const char *sql, size_t sql_len) {
    size_t payload = 1 + sql_len;   /* 0x16 + sql bytes */
    size_t total   = 4 + payload;
    if (cap < total || payload > 0xFFFFFF) return 0;

    mysql_write_pkt_header(buf, (uint32_t)payload, 0);
    buf[4] = 0x16;   /* COM_STMT_PREPARE */
    memcpy(buf + 5, sql, sql_len);
    return (int)total;
}

/* -------------------------------------------------------------------------
 * Encoder: COM_STMT_EXECUTE (seq=0, cmd=0x17)
 * Text-mode params: MYSQL_TYPE_VAR_STRING (0xfd) for all non-null params.
 * ---------------------------------------------------------------------- */

int mysql_encode_com_stmt_execute(uint8_t *buf, size_t cap,
                                  uint32_t stmt_id,
                                  const char **params,
                                  const size_t *param_lens,
                                  int n_params) {
    if (n_params < 0 || n_params > 127) return 0;

    /* Calculate payload size:
       1 (cmd) + 4 (stmt_id) + 1 (cursor) + 4 (iter-count)
       if n_params > 0:
         ceil(n_params/8) (null bitmap) + 1 (new_params_bound_flag)
         + n_params * 2 (type entries)
         + sum of LEI(len) + value for non-null params */
    size_t null_bitmap_len = (n_params > 0) ? (size_t)((n_params + 7) / 8) : 0;

    size_t value_bytes = 0;
    if (n_params > 0) {
        for (int i = 0; i < n_params; i++) {
            if (params[i] != NULL) {
                size_t vlen = param_lens[i];
                /* LEI size for vlen */
                size_t lei_sz = (vlen < 251) ? 1 :
                                (vlen < 0x10000) ? 3 :
                                (vlen < 0x1000000) ? 4 : 9;
                value_bytes += lei_sz + vlen;
            }
        }
    }

    size_t payload = 1 + 4 + 1 + 4;
    if (n_params > 0) {
        payload += null_bitmap_len + 1 + (size_t)n_params * 2 + value_bytes;
    }

    size_t total = 4 + payload;
    if (cap < total || payload > 0xFFFFFF) return 0;

    mysql_write_pkt_header(buf, (uint32_t)payload, 0);
    uint8_t *p = buf + 4;

    *p++ = 0x17;                        /* COM_STMT_EXECUTE */
    wr_le32(p, stmt_id); p += 4;
    *p++ = 0x00;                        /* cursor type: no cursor */
    wr_le32(p, 1); p += 4;             /* iteration-count = 1 */

    if (n_params > 0) {
        /* null bitmap: bit i set if params[i] == NULL */
        uint8_t *bitmap = p;
        memset(bitmap, 0, null_bitmap_len);
        for (int i = 0; i < n_params; i++) {
            if (params[i] == NULL)
                bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
        }
        p += null_bitmap_len;

        *p++ = 0x01;   /* new_params_bound_flag */

        /* type entries: 2 bytes each (type=0xfd, unsigned_flag=0x00) */
        for (int i = 0; i < n_params; i++) {
            *p++ = 0xfd;   /* MYSQL_TYPE_VAR_STRING */
            *p++ = 0x00;   /* unsigned flag */
        }

        /* values for non-null params */
        for (int i = 0; i < n_params; i++) {
            if (params[i] != NULL) {
                size_t vlen = param_lens[i];
                size_t remaining = cap - (size_t)(p - buf);
                int lei = mysql_write_lei(p, remaining, (uint64_t)vlen);
                if (!lei) return 0;
                p += lei;
                memcpy(p, params[i], vlen);
                p += vlen;
            }
        }
    }

    return (int)total;
}

/* -------------------------------------------------------------------------
 * Encoder: COM_STMT_CLOSE (seq=0, cmd=0x19)
 * ---------------------------------------------------------------------- */

int mysql_encode_com_stmt_close(uint8_t *buf, size_t cap, uint32_t stmt_id) {
    /* payload: 1 (cmd) + 4 (stmt_id) = 5 bytes */
    if (cap < 9) return 0;
    mysql_write_pkt_header(buf, 5, 0);
    buf[4] = 0x19;   /* COM_STMT_CLOSE */
    wr_le32(buf + 5, stmt_id);
    return 9;
}

/* -------------------------------------------------------------------------
 * Internal "prepared execute" blob magic
 *
 * Layout:
 *   [0..3]  0xFF 0xFF 0xFF 0xEE  (magic)
 *   [4..7]  sql_len (uint32 LE)
 *   [8..]   sql bytes
 *   [8+sql_len]  n_params (uint8)
 *   for each param:
 *     [0]  type: 0x00=NULL, 0x01=string
 *     if type==0x01:
 *       [1..4]  value_len (uint32 LE)
 *       [5..]   value bytes
 * ---------------------------------------------------------------------- */

static const uint8_t prepared_request_magic[4] = { 0xFF, 0xFF, 0xFF, 0xEE };

int mysql_is_prepared_request(const uint8_t *buf, size_t len) {
    if (len < 4) return 0;
    return memcmp(buf, prepared_request_magic, 4) == 0 ? 1 : 0;
}

int mysql_encode_prepared_request(uint8_t *buf, size_t cap,
                                  const char *sql, size_t sql_len,
                                  const char **params,
                                  const size_t *param_lens,
                                  int n_params) {
    if (n_params < 0 || n_params > 127) return 0;

    /* Calculate required size */
    size_t needed = 4 + 4 + sql_len + 1;   /* magic + sql_len + sql + n_params */
    for (int i = 0; i < n_params; i++) {
        needed += 1;   /* type byte */
        if (params[i] != NULL) {
            needed += 4 + param_lens[i];   /* value_len(uint32) + value */
        }
    }
    if (cap < needed) return 0;

    uint8_t *p = buf;
    memcpy(p, prepared_request_magic, 4); p += 4;

    wr_le32(p, (uint32_t)sql_len); p += 4;
    memcpy(p, sql, sql_len); p += sql_len;

    *p++ = (uint8_t)n_params;

    for (int i = 0; i < n_params; i++) {
        if (params[i] == NULL) {
            *p++ = 0x00;   /* NULL type */
        } else {
            *p++ = 0x01;   /* string type */
            wr_le32(p, (uint32_t)param_lens[i]); p += 4;
            memcpy(p, params[i], param_lens[i]);
            p += param_lens[i];
        }
    }

    return (int)(p - buf);
}

int mysql_decode_prepared_request_params(const uint8_t *blob, size_t blob_len,
                                         const char **params,
                                         size_t *param_lens, int *n_params) {
    if (!mysql_is_prepared_request(blob, blob_len)) return -1;
    if (blob_len < 4 + 4) return -1;

    const uint8_t *p   = blob + 4;
    size_t         rem = blob_len - 4;

    uint32_t sql_len = rd_le32(p); p += 4; rem -= 4;
    if (rem < (size_t)sql_len) return -1;
    p += sql_len; rem -= sql_len;

    if (rem < 1) return -1;
    int np = (int)*p++; rem--;

    if (np < 0 || np > 127) return -1;
    *n_params = np;

    for (int i = 0; i < np; i++) {
        if (rem < 1) return -1;
        uint8_t type = *p++; rem--;
        if (type == 0x00) {
            params[i]     = NULL;
            param_lens[i] = 0;
        } else if (type == 0x01) {
            if (rem < 4) return -1;
            uint32_t vlen = rd_le32(p); p += 4; rem -= 4;
            if (rem < (size_t)vlen) return -1;
            params[i]     = (const char *)p;
            param_lens[i] = (size_t)vlen;
            p += vlen; rem -= vlen;
        } else {
            return -1;
        }
    }

    return 0;
}
