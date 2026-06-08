#ifndef MYSQL_PACKET_H
#define MYSQL_PACKET_H

/*
 * MySQL Client/Server Protocol wire codec.
 *
 * Packet encoder and decoder for the MySQL handshake, authentication,
 * COM_QUERY, and result-set protocol.  No networking, no wrkx engine
 * headers; safe to link in isolation for unit tests.
 *
 * ADR 0005, Phase 6 (P6-4).
 */

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Capability flags (client → server in HandshakeResponse)
 * ---------------------------------------------------------------------- */

#define MYSQL_CLIENT_LONG_PASSWORD   (1U <<  0)
#define MYSQL_CLIENT_LONG_FLAG       (1U <<  2)
#define MYSQL_CLIENT_CONNECT_WITH_DB (1U <<  3)
#define MYSQL_CLIENT_PROTOCOL_41     (1U <<  9)
#define MYSQL_CLIENT_SECURE_CONN     (1U << 15)
#define MYSQL_CLIENT_PLUGIN_AUTH     (1U << 19)

#define MYSQL_CLIENT_FLAGS_P64 \
    (MYSQL_CLIENT_LONG_PASSWORD | MYSQL_CLIENT_LONG_FLAG | \
     MYSQL_CLIENT_CONNECT_WITH_DB | MYSQL_CLIENT_PROTOCOL_41 | \
     MYSQL_CLIENT_SECURE_CONN | MYSQL_CLIENT_PLUGIN_AUTH)

/* -------------------------------------------------------------------------
 * Packet-type discriminator
 * ---------------------------------------------------------------------- */

typedef enum {
    MYSQL_PKT_UNKNOWN = 0,
    MYSQL_PKT_HANDSHAKE_V10,    /* server greeting; seq=0                    */
    MYSQL_PKT_AUTH_SWITCH_REQ,  /* 0xfe + plugin name + challenge            */
    MYSQL_PKT_AUTH_MORE_DATA,   /* 0x01 + 1-byte marker (0x03 / 0x04)       */
    MYSQL_PKT_OK,               /* 0x00 or Protocol-4.1 0xfe EOF alias       */
    MYSQL_PKT_EOF,              /* 0xfe + 2-byte warnings + 2-byte status    */
    MYSQL_PKT_ERR,              /* 0xff + errno + sqlstate + message         */
    MYSQL_PKT_COLUMN_COUNT,     /* LEI > 0; result-set column count          */
    MYSQL_PKT_COLUMN_DEF,       /* catalog/db/table/name column descriptor   */
    MYSQL_PKT_ROW,              /* raw text row                              */
} mysql_pkt_type;

/* -------------------------------------------------------------------------
 * Parsed packet
 * ---------------------------------------------------------------------- */

typedef struct mysql_parsed_pkt {
    mysql_pkt_type type;
    uint8_t        seq;
    union {
        struct {
            uint8_t  protocol_version;       /* must be 10                  */
            char     server_version[32];
            uint32_t connection_id;
            uint8_t  auth_plugin_data[21];   /* 20 usable bytes + NUL       */
            uint32_t server_capabilities;
            uint8_t  charset;
            uint16_t status_flags;
            char     auth_plugin_name[32];
        } handshake;
        struct {
            char     plugin_name[64];
            uint8_t  auth_data[21];
            uint8_t  auth_data_len;
        } auth_switch;
        struct {
            uint8_t marker;   /* 0x03 = fast-path success; 0x04 = full auth */
        } auth_more_data;
        struct {
            uint64_t affected_rows;
            uint64_t last_insert_id;
            uint16_t status_flags;
            uint16_t warnings;
        } ok;
        struct {
            uint16_t error_code;
            char     sqlstate[6];            /* 5 chars + NUL               */
            char     message[256];
        } err;
        struct {
            uint64_t count;
        } column_count;
        struct {
            char     schema[64];
            char     table[64];
            char     name[64];
            uint32_t type_oid;
        } column_def;
        struct {
            uint16_t warnings;
            uint16_t status_flags;
        } eof;
    };
} mysql_parsed_pkt;

/* -------------------------------------------------------------------------
 * Parse context
 *
 * Resolves the 0x01 ambiguity: AUTH_MORE_DATA in auth phase vs.
 * COLUMN_COUNT=1 in query result phase.
 * ---------------------------------------------------------------------- */

typedef enum {
    MYSQL_CTX_AUTH,     /* connect() handshake loop: 0x01 → AUTH_MORE_DATA  */
    MYSQL_CTX_GENERIC,  /* readable() preamble: 0x01 → COLUMN_COUNT(1)      */
    MYSQL_CTX_COL_DEF,  /* column definition packets                         */
    MYSQL_CTX_ROW,      /* row data packets                                  */
} mysql_ctx;

/* -------------------------------------------------------------------------
 * Packet header primitives
 * ---------------------------------------------------------------------- */

/* Write a 4-byte packet header into buf[0..3]. */
void mysql_write_pkt_header(uint8_t *buf, uint32_t payload_len, uint8_t seq);

/* Parse the 4-byte header.  Returns 0 if fewer than 4 bytes available,
   otherwise fills *out_len and *out_seq and returns 4. */
int mysql_read_pkt_header(const uint8_t *buf, size_t avail,
                          uint32_t *out_len, uint8_t *out_seq);

/* -------------------------------------------------------------------------
 * Length-encoded integer (LEI)
 * ---------------------------------------------------------------------- */

/* Decode LEI at buf[0].  Returns bytes consumed (1, 3, 4, or 9) or 0 if
   buf is too short.  *out is set to UINT64_MAX for the NULL sentinel 0xfb. */
int mysql_read_lei(const uint8_t *buf, size_t avail, uint64_t *out);

/* Encode val as a LEI into buf.  Returns bytes written (1, 3, 4, 9) or 0
   if cap is insufficient. */
int mysql_write_lei(uint8_t *buf, size_t cap, uint64_t val);

/* -------------------------------------------------------------------------
 * Packet parser
 *
 * Returns bytes consumed (> 0) on success, 0 if more data needed,
 * -1 on parse error.  buf must include the 4-byte header.
 * ---------------------------------------------------------------------- */

int mysql_parse_packet(const uint8_t *buf, size_t avail,
                       mysql_ctx ctx, mysql_parsed_pkt *out);

/* -------------------------------------------------------------------------
 * Frontend packet encoders
 *
 * All return bytes written (> 0) or 0 if buffer is too small.
 * All include the 4-byte packet header.
 * ---------------------------------------------------------------------- */

/* HandshakeResponse (seq=1).  auth_plugin_name must be NUL-terminated. */
int mysql_encode_handshake_response(uint8_t *buf, size_t cap,
                                    const char *user, const char *db,
                                    const uint8_t *auth_resp,
                                    uint8_t auth_resp_len,
                                    const char *auth_plugin_name,
                                    uint32_t client_flags);

/* COM_QUERY (seq=0). */
int mysql_encode_com_query(uint8_t *buf, size_t cap,
                           const char *sql, size_t sql_len);

/* COM_QUIT (seq=0). */
int mysql_encode_com_quit(uint8_t *buf, size_t cap);

/* -------------------------------------------------------------------------
 * Auth response helpers
 *
 * challenge is always exactly 20 bytes (auth_plugin_data from HandshakeV10).
 * ---------------------------------------------------------------------- */

/* mysql_native_password: SHA1(pw) XOR SHA1(challenge + SHA1(SHA1(pw))) */
void mysql_native_password(const char *password,
                           const uint8_t challenge[20],
                           uint8_t out[20]);

/* caching_sha2_password fast-path:
   SHA256(pw) XOR SHA256(challenge + SHA256(SHA256(pw))) */
void mysql_sha2_password_fast(const char *password,
                              const uint8_t challenge[20],
                              uint8_t out[32]);

#endif /* MYSQL_PACKET_H */
