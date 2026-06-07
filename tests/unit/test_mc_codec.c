/*
 * tests/unit/test_mc_codec.c
 *
 * Unit tests for the memcached text-protocol codec (ADR 0005, P4-1, t059).
 *
 * Coverage:
 *   - All five command encoders (get, set, delete, incr, decr)
 *   - Buffer-too-small returns -1 for every encoder
 *   - Reply parser: STORED, NOT_STORED, DELETED, NOT_FOUND, END (miss),
 *     VALUE (hit), counter replies, CLIENT_ERROR, SERVER_ERROR, bare ERROR
 *   - Partial-read (PENDING) for no \r\n, incomplete VALUE data, missing END
 *   - Coalesced replies — consumed correctly leaves next reply unread
 *   - Multi-value GET response — first VALUE returned, END consumed
 *   - Zero-length value and zero counter edge cases
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "mc_codec.h"

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Encoder tests
 * ====================================================================== */

void test_encode_get_simple(void) {
    char buf[64];
    int n = mc_encode_get(buf, sizeof(buf), "mykey", 5);
    TEST_ASSERT_EQUAL_INT(13, n);
    TEST_ASSERT_EQUAL_MEMORY("get mykey\r\n", buf, 11);
    /* exact: "get mykey\r\n" = 11 chars */
    TEST_ASSERT_EQUAL_INT(11, n);
}

void test_encode_get_exact(void) {
    char buf[64];
    int n = mc_encode_get(buf, sizeof(buf), "k", 1);
    const char *expected = "get k\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_get_buffer_too_small(void) {
    char buf[4];
    int n = mc_encode_get(buf, sizeof(buf), "mykey", 5);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_encode_set_simple(void) {
    char buf[128];
    int n = mc_encode_set(buf, sizeof(buf), "k", 1, "hello", 5, 0, 0);
    const char *expected = "set k 0 0 5\r\nhello\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_set_flags_and_exptime(void) {
    char buf[128];
    int n = mc_encode_set(buf, sizeof(buf), "key", 3, "val", 3, 7, 300);
    const char *expected = "set key 7 300 3\r\nval\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_set_binary_value(void) {
    /* Value containing \r\n should be transmitted as-is (length-delimited). */
    char buf[128];
    const char val[] = "ab\r\ncd";
    int n = mc_encode_set(buf, sizeof(buf), "k", 1, val, 6, 0, 0);
    TEST_ASSERT_TRUE(n > 0);
    /* header: "set k 0 0 6\r\n" = 13 chars, value: 6, trailing \r\n: 2 */
    TEST_ASSERT_EQUAL_INT(13 + 6 + 2, n);
    TEST_ASSERT_EQUAL_MEMORY(val, buf + 13, 6);
}

void test_encode_set_buffer_too_small(void) {
    char buf[10];
    int n = mc_encode_set(buf, sizeof(buf), "key", 3, "value", 5, 0, 0);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_encode_delete_simple(void) {
    char buf[64];
    int n = mc_encode_delete(buf, sizeof(buf), "mykey", 5);
    const char *expected = "delete mykey\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_delete_buffer_too_small(void) {
    char buf[4];
    int n = mc_encode_delete(buf, sizeof(buf), "k", 1);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_encode_incr_simple(void) {
    char buf[64];
    int n = mc_encode_incr(buf, sizeof(buf), "counter", 7, 1);
    const char *expected = "incr counter 1\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_incr_large_delta(void) {
    char buf[64];
    int n = mc_encode_incr(buf, sizeof(buf), "c", 1, UINT64_C(18446744073709551615));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(memcmp(buf, "incr c ", 7) == 0);
}

void test_encode_incr_buffer_too_small(void) {
    char buf[4];
    int n = mc_encode_incr(buf, sizeof(buf), "k", 1, 1);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_encode_decr_simple(void) {
    char buf[64];
    int n = mc_encode_decr(buf, sizeof(buf), "hits", 4, 5);
    const char *expected = "decr hits 5\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_decr_buffer_too_small(void) {
    char buf[4];
    int n = mc_encode_decr(buf, sizeof(buf), "k", 1, 1);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* =========================================================================
 * Parser — single-line storage / deletion replies
 * ====================================================================== */

static mc_status parse(const char *s, mc_reply *r, size_t *c) {
    return mc_parse_reply(s, strlen(s), r, c);
}

void test_parse_stored(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("STORED\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_STORED, r.type);
    TEST_ASSERT_EQUAL_UINT(8, c);
}

void test_parse_not_stored(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("NOT_STORED\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_NOT_STORED, r.type);
    TEST_ASSERT_EQUAL_UINT(12, c);
}

void test_parse_deleted(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("DELETED\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_DELETED, r.type);
    TEST_ASSERT_EQUAL_UINT(9, c);
}

void test_parse_not_found(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("NOT_FOUND\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_NOT_FOUND, r.type);
    TEST_ASSERT_EQUAL_UINT(11, c);
}

/* =========================================================================
 * Parser — GET responses
 * ====================================================================== */

void test_parse_end_miss(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("END\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_END, r.type);
    TEST_ASSERT_EQUAL_UINT(5, c);
}

void test_parse_value_hit(void) {
    const char *resp = "VALUE mykey 0 5\r\nhello\r\nEND\r\n";
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(resp, strlen(resp), &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, r.type);
    TEST_ASSERT_EQUAL_UINT(5, r.keylen);
    TEST_ASSERT_EQUAL_MEMORY("mykey", r.key, 5);
    TEST_ASSERT_EQUAL_UINT(0, r.flags);
    TEST_ASSERT_EQUAL_UINT(5, r.datalen);
    TEST_ASSERT_EQUAL_MEMORY("hello", r.data, 5);
    TEST_ASSERT_EQUAL_UINT(strlen(resp), c);
}

void test_parse_value_with_flags(void) {
    const char *resp = "VALUE k 42 3\r\nfoo\r\nEND\r\n";
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(resp, strlen(resp), &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, r.type);
    TEST_ASSERT_EQUAL_UINT(42, r.flags);
    TEST_ASSERT_EQUAL_UINT(3, r.datalen);
    TEST_ASSERT_EQUAL_MEMORY("foo", r.data, 3);
}

void test_parse_value_zero_length(void) {
    /* SET with empty value → VALUE key 0 0\r\n\r\nEND\r\n */
    const char *resp = "VALUE k 0 0\r\n\r\nEND\r\n";
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(resp, strlen(resp), &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, r.type);
    TEST_ASSERT_EQUAL_UINT(0, r.datalen);
    TEST_ASSERT_EQUAL_UINT(strlen(resp), c);
}

void test_parse_value_binary_data(void) {
    /* Value that contains \r\n internally — byte count keeps it safe. */
    const char  hdr[]  = "VALUE k 0 6\r\n";
    const char  dat[]  = "ab\r\ncd";
    const char  tail[] = "\r\nEND\r\n";
    char resp[64];
    size_t hlen = strlen(hdr), tlen = strlen(tail);
    memcpy(resp,               hdr,  hlen);
    memcpy(resp + hlen,        dat,  6);
    memcpy(resp + hlen + 6,    tail, tlen);
    size_t total = hlen + 6 + tlen;

    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(resp, total, &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, r.type);
    TEST_ASSERT_EQUAL_UINT(6, r.datalen);
    TEST_ASSERT_EQUAL_MEMORY(dat, r.data, 6);
    TEST_ASSERT_EQUAL_UINT(total, c);
}

void test_parse_value_multi_block(void) {
    /* Two VALUE blocks; parser returns the first, consumed covers both + END. */
    const char *resp =
        "VALUE k1 0 2\r\nhi\r\n"
        "VALUE k2 0 5\r\nhello\r\n"
        "END\r\n";
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(resp, strlen(resp), &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, r.type);
    TEST_ASSERT_EQUAL_UINT(2, r.keylen);
    TEST_ASSERT_EQUAL_MEMORY("k1", r.key, 2);
    TEST_ASSERT_EQUAL_UINT(strlen(resp), c);
}

/* =========================================================================
 * Parser — counter replies
 * ====================================================================== */

void test_parse_counter_nonzero(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("42\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_COUNTER, r.type);
    TEST_ASSERT_EQUAL_UINT64(42, r.counter);
    TEST_ASSERT_EQUAL_UINT(4, c);
}

void test_parse_counter_zero(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("0\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_COUNTER, r.type);
    TEST_ASSERT_EQUAL_UINT64(0, r.counter);
}

void test_parse_counter_large(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("18446744073709551615\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_COUNTER, r.type);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(18446744073709551615), r.counter);
}

/* =========================================================================
 * Parser — error replies
 * ====================================================================== */

void test_parse_client_error(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse("CLIENT_ERROR bad command line format\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_CLIENT_ERR, r.type);
    TEST_ASSERT_EQUAL_UINT(strlen("bad command line format"), r.errlen);
    TEST_ASSERT_EQUAL_MEMORY("bad command line format", r.errmsg, r.errlen);
}

void test_parse_client_error_no_message(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse("CLIENT_ERROR\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_CLIENT_ERR, r.type);
    TEST_ASSERT_EQUAL_UINT(0, r.errlen);
}

void test_parse_server_error(void) {
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse("SERVER_ERROR out of memory\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_SERVER_ERR, r.type);
    TEST_ASSERT_EQUAL_UINT(strlen("out of memory"), r.errlen);
    TEST_ASSERT_EQUAL_MEMORY("out of memory", r.errmsg, r.errlen);
}

void test_parse_bare_error(void) {
    /* Bare "ERROR\r\n" (unrecognised command) → SERVER_ERR, empty message */
    mc_reply r; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse("ERROR\r\n", &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_SERVER_ERR, r.type);
    TEST_ASSERT_EQUAL_UINT(0, r.errlen);
    TEST_ASSERT_EQUAL_UINT(7, c);
}

/* =========================================================================
 * Parser — partial / PENDING cases
 * ====================================================================== */

void test_parse_pending_no_crlf(void) {
    mc_reply r; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_parse_reply("STORED", 6, &r, &c));
}

void test_parse_pending_partial_stored(void) {
    mc_reply r; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_parse_reply("STO", 3, &r, &c));
}

void test_parse_pending_value_data_incomplete(void) {
    /* Header arrived but data bytes are missing */
    const char *s = "VALUE k 0 5\r\nhel";
    mc_reply r; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_parse_reply(s, strlen(s), &r, &c));
}

void test_parse_pending_value_no_end(void) {
    /* Full data block present but END\r\n not yet received */
    const char *s = "VALUE k 0 5\r\nhello\r\n";
    mc_reply r; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_parse_reply(s, strlen(s), &r, &c));
}

void test_parse_pending_empty_buffer(void) {
    mc_reply r; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_parse_reply("", 0, &r, &c));
}

/* =========================================================================
 * Parser — coalesced replies
 * ====================================================================== */

void test_parse_coalesced_stored_deleted(void) {
    /* Two replies back-to-back; parse the first, then the second at offset. */
    const char *buf = "STORED\r\nDELETED\r\n";
    size_t      len = strlen(buf);
    mc_reply r; size_t c;

    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, mc_parse_reply(buf, len, &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_STORED, r.type);
    TEST_ASSERT_EQUAL_UINT(8, c);

    /* Second reply starts at buf+c */
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(buf + c, len - c, &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_DELETED, r.type);
    TEST_ASSERT_EQUAL_UINT(9, c);
}

void test_parse_coalesced_value_then_stored(void) {
    const char *buf = "VALUE k 0 2\r\nhi\r\nEND\r\nSTORED\r\n";
    size_t      len = strlen(buf);
    mc_reply r; size_t c;

    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, mc_parse_reply(buf, len, &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, r.type);

    /* Remaining buffer starts with "STORED\r\n" */
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_parse_reply(buf + c, len - c, &r, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_STORED, r.type);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    UNITY_BEGIN();

    /* Encoders */
    RUN_TEST(test_encode_get_exact);
    RUN_TEST(test_encode_get_buffer_too_small);
    RUN_TEST(test_encode_set_simple);
    RUN_TEST(test_encode_set_flags_and_exptime);
    RUN_TEST(test_encode_set_binary_value);
    RUN_TEST(test_encode_set_buffer_too_small);
    RUN_TEST(test_encode_delete_simple);
    RUN_TEST(test_encode_delete_buffer_too_small);
    RUN_TEST(test_encode_incr_simple);
    RUN_TEST(test_encode_incr_large_delta);
    RUN_TEST(test_encode_incr_buffer_too_small);
    RUN_TEST(test_encode_decr_simple);
    RUN_TEST(test_encode_decr_buffer_too_small);

    /* Single-line storage/deletion replies */
    RUN_TEST(test_parse_stored);
    RUN_TEST(test_parse_not_stored);
    RUN_TEST(test_parse_deleted);
    RUN_TEST(test_parse_not_found);

    /* GET responses */
    RUN_TEST(test_parse_end_miss);
    RUN_TEST(test_parse_value_hit);
    RUN_TEST(test_parse_value_with_flags);
    RUN_TEST(test_parse_value_zero_length);
    RUN_TEST(test_parse_value_binary_data);
    RUN_TEST(test_parse_value_multi_block);

    /* Counter replies */
    RUN_TEST(test_parse_counter_nonzero);
    RUN_TEST(test_parse_counter_zero);
    RUN_TEST(test_parse_counter_large);

    /* Error replies */
    RUN_TEST(test_parse_client_error);
    RUN_TEST(test_parse_client_error_no_message);
    RUN_TEST(test_parse_server_error);
    RUN_TEST(test_parse_bare_error);

    /* Partial / PENDING */
    RUN_TEST(test_parse_pending_no_crlf);
    RUN_TEST(test_parse_pending_partial_stored);
    RUN_TEST(test_parse_pending_value_data_incomplete);
    RUN_TEST(test_parse_pending_value_no_end);
    RUN_TEST(test_parse_pending_empty_buffer);

    /* Coalesced */
    RUN_TEST(test_parse_coalesced_stored_deleted);
    RUN_TEST(test_parse_coalesced_value_then_stored);

    return UNITY_END();
}
