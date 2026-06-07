/*
 * tests/unit/test_mc_request.c
 *
 * Unit tests for the memcached request model (ADR 0005, P4-1, t060).
 *
 * Coverage:
 *   - mc_request_validate: null, missing key, empty key, key too long,
 *     key with spaces, key with control chars, SET with null value+vallen>0,
 *     valid requests for all five operations
 *   - mc_request_encode: correct wire format for every operation,
 *     invalid request returns -1, buffer too small returns -1
 *   - mc_response_parse: correct field mapping for every reply type,
 *     PENDING propagated correctly
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "mc_request.h"

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Helpers
 * ====================================================================== */

static mc_request make_get(const char *key) {
    mc_request r = {0};
    r.op     = MC_OP_GET;
    r.key    = key;
    r.keylen = strlen(key);
    return r;
}

static mc_request make_set(const char *key, const char *val,
                            uint32_t flags, uint32_t exptime) {
    mc_request r = {0};
    r.op      = MC_OP_SET;
    r.key     = key;
    r.keylen  = strlen(key);
    r.value   = val;
    r.vallen  = strlen(val);
    r.flags   = flags;
    r.exptime = exptime;
    return r;
}

static mc_request make_delete(const char *key) {
    mc_request r = {0};
    r.op     = MC_OP_DELETE;
    r.key    = key;
    r.keylen = strlen(key);
    return r;
}

static mc_request make_incr(const char *key, uint64_t delta) {
    mc_request r = {0};
    r.op     = MC_OP_INCR;
    r.key    = key;
    r.keylen = strlen(key);
    r.delta  = delta;
    return r;
}

static mc_request make_decr(const char *key, uint64_t delta) {
    mc_request r = {0};
    r.op     = MC_OP_DECR;
    r.key    = key;
    r.keylen = strlen(key);
    r.delta  = delta;
    return r;
}

/* =========================================================================
 * mc_request_validate
 * ====================================================================== */

void test_validate_null_req(void) {
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(NULL));
}

void test_validate_null_key(void) {
    mc_request r = make_get("k");
    r.key = NULL;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_empty_key(void) {
    mc_request r = make_get("k");
    r.keylen = 0;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_key_too_long(void) {
    /* Build a 251-byte key (one over MC_KEY_MAX = 250) */
    char key[252];
    memset(key, 'x', 251);
    key[251] = '\0';
    mc_request r = {0};
    r.op     = MC_OP_GET;
    r.key    = key;
    r.keylen = 251;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_key_at_max_length(void) {
    char key[251];
    memset(key, 'x', 250);
    key[250] = '\0';
    mc_request r = {0};
    r.op     = MC_OP_GET;
    r.key    = key;
    r.keylen = 250;
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

void test_validate_key_with_space(void) {
    mc_request r = make_get("bad key");
    r.keylen = 7;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_key_with_control_char(void) {
    const char bad[] = "key\x01";
    mc_request r = {0};
    r.op     = MC_OP_GET;
    r.key    = bad;
    r.keylen = 4;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_key_with_del(void) {
    const char bad[] = "key\x7f";
    mc_request r = {0};
    r.op     = MC_OP_GET;
    r.key    = bad;
    r.keylen = 4;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_set_null_value_nonzero_vallen(void) {
    mc_request r = make_set("k", "v", 0, 0);
    r.value  = NULL;
    r.vallen = 5;
    TEST_ASSERT_EQUAL_INT(-1, mc_request_validate(&r));
}

void test_validate_set_null_value_zero_vallen(void) {
    /* NULL value with vallen==0 is allowed — encodes as empty value */
    mc_request r = make_set("k", "v", 0, 0);
    r.value  = NULL;
    r.vallen = 0;
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

void test_validate_valid_get(void) {
    mc_request r = make_get("mykey");
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

void test_validate_valid_set(void) {
    mc_request r = make_set("k", "hello", 0, 60);
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

void test_validate_valid_delete(void) {
    mc_request r = make_delete("k");
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

void test_validate_valid_incr(void) {
    mc_request r = make_incr("counter", 1);
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

void test_validate_valid_decr(void) {
    mc_request r = make_decr("counter", 5);
    TEST_ASSERT_EQUAL_INT(0, mc_request_validate(&r));
}

/* =========================================================================
 * mc_request_encode
 * ====================================================================== */

void test_encode_get(void) {
    mc_request r = make_get("mykey");
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "get mykey\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_set(void) {
    mc_request r = make_set("k", "hello", 0, 0);
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "set k 0 0 5\r\nhello\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_set_with_flags_exptime(void) {
    mc_request r = make_set("key", "val", 7, 300);
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "set key 7 300 3\r\nval\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_set_empty_value(void) {
    mc_request r = {0};
    r.op     = MC_OP_SET;
    r.key    = "k";
    r.keylen = 1;
    r.value  = NULL;
    r.vallen = 0;
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "set k 0 0 0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_delete(void) {
    mc_request r = make_delete("mykey");
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "delete mykey\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_incr(void) {
    mc_request r = make_incr("hits", 1);
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "incr hits 1\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_decr(void) {
    mc_request r = make_decr("hits", 10);
    char buf[64];
    int n = mc_request_encode(&r, buf, sizeof(buf));
    const char *expected = "decr hits 10\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_encode_invalid_returns_minus1(void) {
    mc_request r = make_get("bad key"); /* space in key */
    r.keylen = 7;
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-1, mc_request_encode(&r, buf, sizeof(buf)));
}

void test_encode_buffer_too_small(void) {
    mc_request r = make_get("mykey");
    char buf[4];
    TEST_ASSERT_EQUAL_INT(-1, mc_request_encode(&r, buf, sizeof(buf)));
}

/* =========================================================================
 * mc_response_parse
 * ====================================================================== */

static mc_status parse_resp(const char *s, mc_response *resp, size_t *c) {
    return mc_response_parse(s, strlen(s), resp, c);
}

void test_response_stored(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse_resp("STORED\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_STORED, resp.status);
    TEST_ASSERT_EQUAL_UINT(8, c);
}

void test_response_not_stored(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse_resp("NOT_STORED\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_NOT_STORED, resp.status);
}

void test_response_deleted(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse_resp("DELETED\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_DELETED, resp.status);
}

void test_response_not_found(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse_resp("NOT_FOUND\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_NOT_FOUND, resp.status);
}

void test_response_end_miss(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse_resp("END\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_END, resp.status);
}

void test_response_value_hit(void) {
    const char *raw = "VALUE mykey 42 5\r\nhello\r\nEND\r\n";
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        mc_response_parse(raw, strlen(raw), &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_VALUE, resp.status);
    TEST_ASSERT_EQUAL_UINT(5,  resp.vallen);
    TEST_ASSERT_EQUAL_UINT(42, resp.flags);
    TEST_ASSERT_EQUAL_MEMORY("hello", resp.value, 5);
}

void test_response_counter(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE, parse_resp("99\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_COUNTER, resp.status);
    TEST_ASSERT_EQUAL_UINT64(99, resp.counter);
}

void test_response_client_error(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse_resp("CLIENT_ERROR bad format\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_CLIENT_ERR, resp.status);
    TEST_ASSERT_EQUAL_UINT(strlen("bad format"), resp.errlen);
    TEST_ASSERT_EQUAL_MEMORY("bad format", resp.errmsg, resp.errlen);
}

void test_response_server_error(void) {
    mc_response resp; size_t c;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_DONE,
        parse_resp("SERVER_ERROR out of memory\r\n", &resp, &c));
    TEST_ASSERT_EQUAL_INT(MC_REPLY_SERVER_ERR, resp.status);
    TEST_ASSERT_EQUAL_UINT(strlen("out of memory"), resp.errlen);
}

void test_response_pending(void) {
    mc_response resp; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_response_parse("STORED", 6, &resp, &c));
}

void test_response_pending_partial_value(void) {
    const char *s = "VALUE k 0 5\r\nhel";
    mc_response resp; size_t c = 0;
    TEST_ASSERT_EQUAL_INT(MC_STATUS_PENDING,
        mc_response_parse(s, strlen(s), &resp, &c));
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    UNITY_BEGIN();

    /* Validation */
    RUN_TEST(test_validate_null_req);
    RUN_TEST(test_validate_null_key);
    RUN_TEST(test_validate_empty_key);
    RUN_TEST(test_validate_key_too_long);
    RUN_TEST(test_validate_key_at_max_length);
    RUN_TEST(test_validate_key_with_space);
    RUN_TEST(test_validate_key_with_control_char);
    RUN_TEST(test_validate_key_with_del);
    RUN_TEST(test_validate_set_null_value_nonzero_vallen);
    RUN_TEST(test_validate_set_null_value_zero_vallen);
    RUN_TEST(test_validate_valid_get);
    RUN_TEST(test_validate_valid_set);
    RUN_TEST(test_validate_valid_delete);
    RUN_TEST(test_validate_valid_incr);
    RUN_TEST(test_validate_valid_decr);

    /* Encoding */
    RUN_TEST(test_encode_get);
    RUN_TEST(test_encode_set);
    RUN_TEST(test_encode_set_with_flags_exptime);
    RUN_TEST(test_encode_set_empty_value);
    RUN_TEST(test_encode_delete);
    RUN_TEST(test_encode_incr);
    RUN_TEST(test_encode_decr);
    RUN_TEST(test_encode_invalid_returns_minus1);
    RUN_TEST(test_encode_buffer_too_small);

    /* Response parsing */
    RUN_TEST(test_response_stored);
    RUN_TEST(test_response_not_stored);
    RUN_TEST(test_response_deleted);
    RUN_TEST(test_response_not_found);
    RUN_TEST(test_response_end_miss);
    RUN_TEST(test_response_value_hit);
    RUN_TEST(test_response_counter);
    RUN_TEST(test_response_client_error);
    RUN_TEST(test_response_server_error);
    RUN_TEST(test_response_pending);
    RUN_TEST(test_response_pending_partial_value);

    return UNITY_END();
}
