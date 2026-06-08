/*
 * tests/unit/test_mysql_codec.c
 *
 * Unit tests for the MySQL wire-protocol codec (ADR 0005, P6-4).
 *
 * Tests mysql_packet.c in isolation: no TCP, no engine, no wrkx core.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "unity.h"
#include "mysql_packet.h"

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Helpers
 * ====================================================================== */

/* Build a complete 4-byte-header + payload packet in buf. */
static void make_pkt(uint8_t *buf, size_t *len,
                     uint8_t seq,
                     const uint8_t *payload, size_t plen) {
    mysql_write_pkt_header(buf, (uint32_t)plen, seq);
    memcpy(buf + 4, payload, plen);
    *len = 4 + plen;
}

/* =========================================================================
 * Packet header
 * ====================================================================== */

void test_pkt_header_roundtrip(void) {
    uint8_t buf[4];
    mysql_write_pkt_header(buf, 0x1A2B3C, 7);
    uint32_t out_len;
    uint8_t  out_seq;
    int rc = mysql_read_pkt_header(buf, 4, &out_len, &out_seq);
    TEST_ASSERT_EQUAL_INT(4, rc);
    TEST_ASSERT_EQUAL_UINT32(0x1A2B3C, out_len);
    TEST_ASSERT_EQUAL_UINT8(7, out_seq);
}

void test_pkt_header_short(void) {
    uint8_t buf[3] = {0x05, 0x00, 0x00};
    uint32_t out_len; uint8_t out_seq;
    TEST_ASSERT_EQUAL_INT(0, mysql_read_pkt_header(buf, 3, &out_len, &out_seq));
}

/* =========================================================================
 * LEI
 * ====================================================================== */

void test_lei_decode_1byte(void) {
    uint8_t b = 0x05;
    uint64_t out;
    TEST_ASSERT_EQUAL_INT(1, mysql_read_lei(&b, 1, &out));
    TEST_ASSERT_EQUAL_UINT64(5, out);
}

void test_lei_decode_2byte(void) {
    uint8_t buf[3] = {0xfc, 0x00, 0x01};
    uint64_t out;
    TEST_ASSERT_EQUAL_INT(3, mysql_read_lei(buf, 3, &out));
    TEST_ASSERT_EQUAL_UINT64(256, out);
}

void test_lei_decode_null(void) {
    uint8_t b = 0xfb;
    uint64_t out;
    TEST_ASSERT_EQUAL_INT(1, mysql_read_lei(&b, 1, &out));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, out);
}

void test_lei_encode_roundtrip(void) {
    uint64_t vals[] = {0, 250, 251, 65535, 65536, 16777215, 16777216};
    for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
        uint8_t buf[9];
        int wn = mysql_write_lei(buf, sizeof(buf), vals[i]);
        TEST_ASSERT_GREATER_THAN_INT(0, wn);
        uint64_t out;
        int rn = mysql_read_lei(buf, (size_t)wn, &out);
        TEST_ASSERT_EQUAL_INT(wn, rn);
        TEST_ASSERT_EQUAL_UINT64(vals[i], out);
    }
}

/* =========================================================================
 * Encoders
 * ====================================================================== */

void test_encode_com_query_format(void) {
    uint8_t buf[64];
    const char *sql = "SELECT 1";
    int n = mysql_encode_com_query(buf, sizeof(buf), sql, strlen(sql));
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    uint32_t plen; uint8_t seq;
    mysql_read_pkt_header(buf, 4, &plen, &seq);
    TEST_ASSERT_EQUAL_UINT32(9, plen);   /* 1 (COM_QUERY) + 8 (sql) */
    TEST_ASSERT_EQUAL_UINT8(0, seq);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[4]);
    TEST_ASSERT_EQUAL_MEMORY("SELECT 1", buf + 5, 8);
}

void test_encode_com_quit_format(void) {
    uint8_t buf[8];
    int n = mysql_encode_com_quit(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, n);
    uint32_t plen; uint8_t seq;
    mysql_read_pkt_header(buf, 4, &plen, &seq);
    TEST_ASSERT_EQUAL_UINT32(1, plen);
    TEST_ASSERT_EQUAL_UINT8(0, seq);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[4]);
}

void test_encode_handshake_response_structure(void) {
    uint8_t auth_resp[20];
    memset(auth_resp, 0xAB, 20);

    uint8_t buf[256];
    int n = mysql_encode_handshake_response(
                buf, sizeof(buf),
                "wrkx", "test",
                auth_resp, 20,
                "mysql_native_password",
                MYSQL_CLIENT_FLAGS_P64);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    /* seq must be 1 */
    uint32_t plen; uint8_t seq;
    mysql_read_pkt_header(buf, 4, &plen, &seq);
    TEST_ASSERT_EQUAL_UINT8(1, seq);

    /* Capabilities are at offset 4 (first 4 bytes of payload) */
    uint8_t *p = buf + 4;
    uint32_t caps = (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
                    ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
    TEST_ASSERT_TRUE((caps & MYSQL_CLIENT_PROTOCOL_41) != 0);
    TEST_ASSERT_TRUE((caps & MYSQL_CLIENT_PLUGIN_AUTH) != 0);

    /* Scan payload for user "wrkx\0" */
    uint8_t *payload_end = buf + 4 + plen;
    uint8_t *found_user = (uint8_t *)memmem(buf + 4, plen, "wrkx\0", 5);
    TEST_ASSERT_NOT_NULL(found_user);

    /* db "test\0" must follow auth data somewhere after user */
    uint8_t *found_db = (uint8_t *)memmem(found_user, (size_t)(payload_end - found_user), "test\0", 5);
    TEST_ASSERT_NOT_NULL(found_db);

    /* plugin name "mysql_native_password\0" must follow db\0 */
    uint8_t *found_plugin = (uint8_t *)memmem(
        found_db, (size_t)(payload_end - found_db),
        "mysql_native_password\0", 22);
    TEST_ASSERT_NOT_NULL(found_plugin);
}

/* =========================================================================
 * Auth packet parsing (MYSQL_CTX_AUTH)
 * ====================================================================== */

void test_parse_auth_more_data_fast_success(void) {
    uint8_t payload[2] = {0x01, 0x03};
    uint8_t pkt[6]; size_t plen;
    make_pkt(pkt, &plen, 2, payload, 2);

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_AUTH, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_AUTH_MORE_DATA, out.type);
    TEST_ASSERT_EQUAL_UINT8(0x03, out.auth_more_data.marker);
}

void test_parse_auth_more_data_full_auth(void) {
    uint8_t payload[2] = {0x01, 0x04};
    uint8_t pkt[6]; size_t plen;
    make_pkt(pkt, &plen, 2, payload, 2);

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_AUTH, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_AUTH_MORE_DATA, out.type);
    TEST_ASSERT_EQUAL_UINT8(0x04, out.auth_more_data.marker);
}

void test_parse_ok_packet(void) {
    /* OK: 0x00 + LEI(0) + LEI(0) + status(2) + warnings(2) */
    uint8_t payload[] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    uint8_t pkt[32]; size_t plen;
    make_pkt(pkt, &plen, 1, payload, sizeof(payload));

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_AUTH, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_OK, out.type);
    TEST_ASSERT_EQUAL_UINT64(0, out.ok.affected_rows);
    TEST_ASSERT_EQUAL_UINT64(0, out.ok.last_insert_id);
}

void test_parse_err_packet(void) {
    /* ERR: 0xff + errno(2LE) + '#' + sqlstate(5) + message */
    uint8_t payload[] = {
        0xff,
        0x48, 0x04,           /* errno = 0x0448 = 1096 */
        '#',
        'H', 'Y', '0', '0', '0',
        'e', 'r', 'r', 'o', 'r', ' ', 't', 'e', 'x', 't'
    };
    uint8_t pkt[64]; size_t plen;
    make_pkt(pkt, &plen, 0, payload, sizeof(payload));

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_AUTH, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_ERR, out.type);
    TEST_ASSERT_EQUAL_UINT16(0x0448, out.err.error_code);
    TEST_ASSERT_EQUAL_MEMORY("HY000", out.err.sqlstate, 5);
    TEST_ASSERT_EQUAL_MEMORY("error text", out.err.message, 10);
}

/* =========================================================================
 * Result-set preamble parsing (MYSQL_CTX_GENERIC)
 * ====================================================================== */

void test_parse_column_count_preamble_single(void) {
    /* 0x01 in GENERIC context = column count 1, NOT AUTH_MORE_DATA */
    uint8_t payload[] = {0x01};
    uint8_t pkt[8]; size_t plen;
    make_pkt(pkt, &plen, 1, payload, 1);

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_GENERIC, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_COLUMN_COUNT, out.type);
    TEST_ASSERT_EQUAL_UINT64(1, out.column_count.count);
}

void test_parse_column_count_preamble(void) {
    uint8_t payload[] = {0x03};
    uint8_t pkt[8]; size_t plen;
    make_pkt(pkt, &plen, 1, payload, 1);

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_GENERIC, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_COLUMN_COUNT, out.type);
    TEST_ASSERT_EQUAL_UINT64(3, out.column_count.count);
}

void test_parse_eof_packet(void) {
    /* EOF: 0xfe + warnings(2) + status(2) — 5 bytes, payload_len <= 9 */
    uint8_t payload[] = {0xfe, 0x01, 0x00, 0x02, 0x00};
    uint8_t pkt[16]; size_t plen;
    make_pkt(pkt, &plen, 5, payload, sizeof(payload));

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_GENERIC, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_EOF, out.type);
    TEST_ASSERT_EQUAL_UINT16(1, out.eof.warnings);
    TEST_ASSERT_EQUAL_UINT16(2, out.eof.status_flags);
}

/* =========================================================================
 * Column definition (MYSQL_CTX_COL_DEF)
 * ====================================================================== */

void test_parse_column_def(void) {
    /*
     * Minimal column definition:
     *   catalog: LEI(0) -> just 0x00
     *   schema:  LEI(0)
     *   table:   LEI(0)
     *   org_table: LEI(0)
     *   name:    LEI(4) + "name"
     *   org_name: LEI(0)
     *   0x0c + charset(2) + col_length(4) + type(1) + flags(2) + dec(1) + pad(2)
     */
    uint8_t payload[] = {
        0x00,               /* catalog len=0 */
        0x00,               /* schema len=0 */
        0x00,               /* table len=0 */
        0x00,               /* org_table len=0 */
        0x04, 'n','a','m','e',  /* name len=4 + "name" */
        0x00,               /* org_name len=0 */
        0x0c,               /* fixed length marker */
        0x08, 0x00,         /* charset utf8mb4 */
        0x0b, 0x00, 0x00, 0x00, /* col_length */
        0xfd,               /* type: VAR_STRING */
        0x01, 0x00,         /* flags */
        0x00,               /* decimals */
        0x00, 0x00          /* padding */
    };
    uint8_t pkt[64]; size_t plen;
    make_pkt(pkt, &plen, 2, payload, sizeof(payload));

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_COL_DEF, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_COLUMN_DEF, out.type);
    TEST_ASSERT_EQUAL_MEMORY("name", out.column_def.name, 4);
    TEST_ASSERT_EQUAL_UINT32(0xfd, out.column_def.type_oid);
}

/* =========================================================================
 * Row (MYSQL_CTX_ROW)
 * ====================================================================== */

void test_parse_row_generic(void) {
    uint8_t payload[] = {0x31, 0x32, 0x33};   /* "123" — a text row */
    uint8_t pkt[16]; size_t plen;
    make_pkt(pkt, &plen, 3, payload, sizeof(payload));

    mysql_parsed_pkt out;
    int rc = mysql_parse_packet(pkt, plen, MYSQL_CTX_ROW, &out);
    TEST_ASSERT_EQUAL_INT((int)plen, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_ROW, out.type);
}

/* =========================================================================
 * Incomplete returns zero
 * ====================================================================== */

void test_parse_incomplete_returns_zero(void) {
    /* A complete OK packet is 11 bytes; truncate to 6 */
    uint8_t payload[] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    uint8_t full[32]; size_t plen;
    make_pkt(full, &plen, 1, payload, sizeof(payload));

    mysql_parsed_pkt out;
    /* Try every prefix length shorter than total */
    for (size_t trunc = 0; trunc < plen; trunc++) {
        int rc = mysql_parse_packet(full, trunc, MYSQL_CTX_AUTH, &out);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "expected 0 for truncated input");
    }
}

/* =========================================================================
 * Auth vector tests
 * ====================================================================== */

void test_native_password_known_vector(void) {
    /*
     * Reference: mysql_native_password("secret", challenge)
     * where challenge = bytes 0x01..0x14 (1..20).
     *
     * Computed offline via Python:
     *   import hashlib, binascii
     *   pw = b"secret"
     *   ch = bytes(range(1, 21))
     *   s1 = hashlib.sha1(pw).digest()
     *   s2 = hashlib.sha1(s1).digest()
     *   tok = hashlib.sha1(ch + s2).digest()
     *   result = bytes(a^b for a,b in zip(s1, tok))
     */
    static const uint8_t challenge[20] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20
    };
    static const uint8_t expected[20] = {
        0xb3,0x2b,0xb3,0xa5,0x83,0xe1,0x34,0x0c,0x0a,0x11,
        0x08,0xd5,0x8b,0x1b,0xe4,0x97,0x81,0xad,0x8c,0x2f
    };
    uint8_t out[20];
    mysql_native_password("secret", challenge, out);
    TEST_ASSERT_EQUAL_MEMORY(expected, out, 20);
}

void test_sha2_fast_path_known_vector(void) {
    /*
     * Reference: caching_sha2_password fast-path ("secret", challenge)
     * where challenge = bytes 0x01..0x14 (1..20).
     *
     * Computed offline via Python:
     *   import hashlib
     *   pw = b"secret"
     *   ch = bytes(range(1, 21))
     *   s1 = hashlib.sha256(pw).digest()
     *   s2 = hashlib.sha256(s1).digest()
     *   tok = hashlib.sha256(s2 + ch).digest()
     *   result = bytes(a^b for a,b in zip(s1, tok))
     */
    static const uint8_t challenge[20] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20
    };
    static const uint8_t expected[32] = {
        0x74,0x6e,0xbe,0x20,0x5d,0x56,0xa0,0x70,
        0x7a,0xcb,0x3e,0x79,0x6e,0x83,0x4e,0x0d,
        0xd7,0xb1,0xd6,0x17,0x43,0xb2,0x6b,0xd5,
        0x20,0x2c,0x7a,0x62,0x32,0x30,0xc7,0xc9
    };
    uint8_t out[32];
    mysql_sha2_password_fast("secret", challenge, out);
    TEST_ASSERT_EQUAL_MEMORY(expected, out, 32);
}

/* =========================================================================
 * Main
 * ====================================================================== */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_pkt_header_roundtrip);
    RUN_TEST(test_pkt_header_short);

    RUN_TEST(test_lei_decode_1byte);
    RUN_TEST(test_lei_decode_2byte);
    RUN_TEST(test_lei_decode_null);
    RUN_TEST(test_lei_encode_roundtrip);

    RUN_TEST(test_encode_com_query_format);
    RUN_TEST(test_encode_com_quit_format);
    RUN_TEST(test_encode_handshake_response_structure);

    RUN_TEST(test_parse_auth_more_data_fast_success);
    RUN_TEST(test_parse_auth_more_data_full_auth);
    RUN_TEST(test_parse_ok_packet);
    RUN_TEST(test_parse_err_packet);

    RUN_TEST(test_parse_column_count_preamble_single);
    RUN_TEST(test_parse_column_count_preamble);
    RUN_TEST(test_parse_eof_packet);
    RUN_TEST(test_parse_column_def);
    RUN_TEST(test_parse_row_generic);
    RUN_TEST(test_parse_incomplete_returns_zero);

    RUN_TEST(test_native_password_known_vector);
    RUN_TEST(test_sha2_fast_path_known_vector);

    return UNITY_END();
}
