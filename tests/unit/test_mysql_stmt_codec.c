/*
 * tests/unit/test_mysql_stmt_codec.c
 *
 * Unit tests for MySQL prepared-statement codec (ADR 0005, P6-5).
 *
 * Tests COM_STMT_PREPARE, COM_STMT_EXECUTE, COM_STMT_CLOSE encoders,
 * COM_STMT_PREPARE_OK parser, BINARY_ROW parser, and the internal
 * prepared-request blob format.
 *
 * No networking, no LuaJIT.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "mysql_packet.h"

void setUp(void)    {}
void tearDown(void) {}

/* -------------------------------------------------------------------------
 * COM_STMT_PREPARE encoding
 * ---------------------------------------------------------------------- */

void test_encode_com_stmt_prepare_format(void) {
    uint8_t buf[64];
    int n = mysql_encode_com_stmt_prepare(buf, sizeof(buf), "SELECT ?", 8);
    TEST_ASSERT_GREATER_THAN(0, n);
    /* header: payload_len = 1 + 8 = 9, seq = 0 */
    uint32_t plen = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16);
    TEST_ASSERT_EQUAL_UINT32(9, plen);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]);   /* seq=0 */
    TEST_ASSERT_EQUAL_UINT8(0x16, buf[4]);   /* COM_STMT_PREPARE */
    TEST_ASSERT_EQUAL_MEMORY("SELECT ?", buf + 5, 8);
    TEST_ASSERT_EQUAL_INT(4 + 9, n);
}

/* -------------------------------------------------------------------------
 * COM_STMT_CLOSE encoding
 * ---------------------------------------------------------------------- */

void test_encode_com_stmt_close_format(void) {
    uint8_t buf[32];
    int n = mysql_encode_com_stmt_close(buf, sizeof(buf), 42);
    TEST_ASSERT_EQUAL_INT(9, n);
    uint32_t plen = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16);
    TEST_ASSERT_EQUAL_UINT32(5, plen);    /* payload = cmd(1) + stmt_id(4) */
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]);   /* seq=0 */
    TEST_ASSERT_EQUAL_UINT8(0x19, buf[4]);   /* COM_STMT_CLOSE */
    uint32_t stmt_id = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) |
                       ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 24);
    TEST_ASSERT_EQUAL_UINT32(42, stmt_id);
}

/* -------------------------------------------------------------------------
 * COM_STMT_EXECUTE — no params
 * ---------------------------------------------------------------------- */

void test_encode_com_stmt_execute_no_params(void) {
    uint8_t buf[64];
    int n = mysql_encode_com_stmt_execute(buf, sizeof(buf), 7, NULL, NULL, 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    /* payload: cmd(1)+stmt_id(4)+cursor(1)+iter(4) = 10 */
    uint32_t plen = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16);
    TEST_ASSERT_EQUAL_UINT32(10, plen);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]);   /* seq=0 */
    TEST_ASSERT_EQUAL_UINT8(0x17, buf[4]);   /* COM_STMT_EXECUTE */
    /* stmt_id at [5..8] */
    uint32_t stmt_id = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) |
                       ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 24);
    TEST_ASSERT_EQUAL_UINT32(7, stmt_id);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[9]);   /* cursor = no cursor */
    /* iter-count at [10..13] = 1 LE */
    uint32_t iter = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8) |
                    ((uint32_t)buf[12] << 16) | ((uint32_t)buf[13] << 24);
    TEST_ASSERT_EQUAL_UINT32(1, iter);
}

/* -------------------------------------------------------------------------
 * COM_STMT_EXECUTE — string param
 * ---------------------------------------------------------------------- */

void test_encode_com_stmt_execute_string_param(void) {
    uint8_t buf[128];
    const char *params[] = { "hello" };
    size_t      lens[]   = { 5 };
    int n = mysql_encode_com_stmt_execute(buf, sizeof(buf), 3, params, lens, 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    /* payload: 10 (fixed) + 1 (null bitmap) + 1 (bound flag) + 2 (type) +
                1 (LEI for 5) + 5 (value) = 20 */
    uint32_t plen = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16);
    TEST_ASSERT_EQUAL_UINT32(20, plen);
    /* null bitmap = 0x00 (not null) at buf[14] */
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[14]);
    /* new_params_bound_flag = 0x01 at buf[15] */
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[15]);
    /* type entry: 0xfd 0x00 at buf[16..17] */
    TEST_ASSERT_EQUAL_UINT8(0xfd, buf[16]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[17]);
    /* LEI(5) = 0x05 at buf[18]; value "hello" at buf[19..23] */
    TEST_ASSERT_EQUAL_UINT8(0x05, buf[18]);
    TEST_ASSERT_EQUAL_MEMORY("hello", buf + 19, 5);
}

/* -------------------------------------------------------------------------
 * COM_STMT_EXECUTE — null param
 * ---------------------------------------------------------------------- */

void test_encode_com_stmt_execute_null_param(void) {
    uint8_t buf[128];
    const char *params[] = { NULL };
    size_t      lens[]   = { 0 };
    int n = mysql_encode_com_stmt_execute(buf, sizeof(buf), 1, params, lens, 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    /* payload: 10 + 1 (bitmap) + 1 (bound) + 2 (type) = 14; no value bytes */
    uint32_t plen = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16);
    TEST_ASSERT_EQUAL_UINT32(14, plen);
    /* null bitmap bit 0 set = 0x01 */
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[14]);
    /* bound flag = 0x01 */
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[15]);
    /* type entry still present: 0xfd 0x00 */
    TEST_ASSERT_EQUAL_UINT8(0xfd, buf[16]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[17]);
    /* no value bytes */
    TEST_ASSERT_EQUAL_INT(4 + 14, n);
}

/* -------------------------------------------------------------------------
 * COM_STMT_EXECUTE — two mixed params (string + null)
 * ---------------------------------------------------------------------- */

void test_encode_com_stmt_execute_two_mixed_params(void) {
    uint8_t buf[128];
    const char *params[] = { "foo", NULL };
    size_t      lens[]   = { 3, 0 };
    int n = mysql_encode_com_stmt_execute(buf, sizeof(buf), 2, params, lens, 2);
    TEST_ASSERT_GREATER_THAN(0, n);
    /* null bitmap: 1 byte; bit 1 set for second param = 0x02 */
    TEST_ASSERT_EQUAL_UINT8(0x02, buf[14]);
    /* bound flag */
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[15]);
    /* two type entries */
    TEST_ASSERT_EQUAL_UINT8(0xfd, buf[16]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[17]);
    TEST_ASSERT_EQUAL_UINT8(0xfd, buf[18]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[19]);
    /* value for "foo": LEI(3) + "foo" */
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[20]);
    TEST_ASSERT_EQUAL_MEMORY("foo", buf + 21, 3);
}

/* -------------------------------------------------------------------------
 * Parse COM_STMT_PREPARE_OK
 * ---------------------------------------------------------------------- */

void test_parse_stmt_prepare_ok(void) {
    /* Build a minimal PREPARE_OK payload:
       0x00 + stmt_id(4) + n_columns(2) + n_params(2) + reserved(1) + warnings(2) */
    uint8_t raw[4 + 12];
    /* header: payload_len=12, seq=1 */
    raw[0] = 12; raw[1] = 0; raw[2] = 0;   /* payload_len LE */
    raw[3] = 1;                              /* seq */
    raw[4] = 0x00;                           /* OK marker */
    /* stmt_id = 5 LE */
    raw[5] = 5; raw[6] = 0; raw[7] = 0; raw[8] = 0;
    /* n_columns = 1 LE */
    raw[9] = 1; raw[10] = 0;
    /* n_params = 1 LE */
    raw[11] = 1; raw[12] = 0;
    /* reserved */
    raw[13] = 0;
    /* warning_count = 0 LE */
    raw[14] = 0; raw[15] = 0;

    mysql_parsed_pkt pkt;
    int rc = mysql_parse_packet(raw, sizeof(raw), MYSQL_CTX_STMT_PREPARE, &pkt);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_STMT_PREPARE_OK, (int)pkt.type);
    TEST_ASSERT_EQUAL_UINT32(5, pkt.stmt_prepare_ok.stmt_id);
    TEST_ASSERT_EQUAL_UINT16(1, pkt.stmt_prepare_ok.n_columns);
    TEST_ASSERT_EQUAL_UINT16(1, pkt.stmt_prepare_ok.n_params);
}

/* -------------------------------------------------------------------------
 * Parse BINARY_ROW (generic non-EOF/non-ERR packet in BINARY_ROW context)
 * ---------------------------------------------------------------------- */

void test_parse_binary_row_generic(void) {
    /* 3-byte payload: not 0xfe / 0xff → BINARY_ROW */
    uint8_t raw[4 + 3];
    raw[0] = 3; raw[1] = 0; raw[2] = 0;   /* payload_len = 3 */
    raw[3] = 2;                             /* seq */
    raw[4] = 0x00;   /* null bitmap marker for binary rows */
    raw[5] = 0xAB;
    raw[6] = 0xCD;

    mysql_parsed_pkt pkt;
    int rc = mysql_parse_packet(raw, sizeof(raw), MYSQL_CTX_BINARY_ROW, &pkt);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_INT(MYSQL_PKT_BINARY_ROW, (int)pkt.type);
}

/* -------------------------------------------------------------------------
 * Prepared-request blob: encode → decode roundtrip
 * ---------------------------------------------------------------------- */

void test_encode_prepared_request_roundtrip(void) {
    uint8_t buf[256];
    const char *sql       = "SELECT ?";
    size_t      sql_len   = 8;
    const char *params[]  = { "val" };
    size_t      lens[]    = { 3 };

    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          sql, sql_len, params, lens, 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_INT(1, mysql_is_prepared_request(buf, (size_t)n));

    const char *out_params[128]; size_t out_lens[128]; int np;
    int rc = mysql_decode_prepared_request_params(buf, (size_t)n,
                                                  out_params, out_lens, &np);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, np);
    TEST_ASSERT_NOT_NULL(out_params[0]);
    TEST_ASSERT_EQUAL_size_t(3, out_lens[0]);
    TEST_ASSERT_EQUAL_MEMORY("val", out_params[0], 3);
}

/* -------------------------------------------------------------------------
 * Prepared-request blob: null param roundtrip
 * ---------------------------------------------------------------------- */

void test_encode_prepared_request_null_param_roundtrip(void) {
    uint8_t buf[128];
    const char *sql      = "SELECT ?";
    size_t      sql_len  = 8;
    const char *params[] = { NULL };
    size_t      lens[]   = { 0 };

    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          sql, sql_len, params, lens, 1);
    TEST_ASSERT_GREATER_THAN(0, n);

    const char *out_params[128]; size_t out_lens[128]; int np;
    int rc = mysql_decode_prepared_request_params(buf, (size_t)n,
                                                  out_params, out_lens, &np);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, np);
    TEST_ASSERT_NULL(out_params[0]);
}

/* -------------------------------------------------------------------------
 * Prepared-request blob: no params
 * ---------------------------------------------------------------------- */

void test_encode_prepared_request_no_params(void) {
    uint8_t buf[128];
    const char *sql     = "SELECT 1";
    size_t      sql_len = 8;

    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          sql, sql_len, NULL, NULL, 0);
    TEST_ASSERT_GREATER_THAN(0, n);

    const char *out_params[128]; size_t out_lens[128]; int np;
    int rc = mysql_decode_prepared_request_params(buf, (size_t)n,
                                                  out_params, out_lens, &np);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(0, np);
}

/* -------------------------------------------------------------------------
 * mysql_is_prepared_request
 * ---------------------------------------------------------------------- */

void test_is_prepared_request_true(void) {
    uint8_t buf[64];
    int n = mysql_encode_prepared_request(buf, sizeof(buf),
                                          "SELECT 1", 8, NULL, NULL, 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_INT(1, mysql_is_prepared_request(buf, (size_t)n));
}

void test_is_prepared_request_false_com_query(void) {
    uint8_t buf[64];
    int n = mysql_encode_com_query(buf, sizeof(buf), "SELECT 1", 8);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_INT(0, mysql_is_prepared_request(buf, (size_t)n));
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_encode_com_stmt_prepare_format);
    RUN_TEST(test_encode_com_stmt_close_format);
    RUN_TEST(test_encode_com_stmt_execute_no_params);
    RUN_TEST(test_encode_com_stmt_execute_string_param);
    RUN_TEST(test_encode_com_stmt_execute_null_param);
    RUN_TEST(test_encode_com_stmt_execute_two_mixed_params);
    RUN_TEST(test_parse_stmt_prepare_ok);
    RUN_TEST(test_parse_binary_row_generic);
    RUN_TEST(test_encode_prepared_request_roundtrip);
    RUN_TEST(test_encode_prepared_request_null_param_roundtrip);
    RUN_TEST(test_encode_prepared_request_no_params);
    RUN_TEST(test_is_prepared_request_true);
    RUN_TEST(test_is_prepared_request_false_com_query);

    return UNITY_END();
}
