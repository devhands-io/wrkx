/*
 * Unit tests for the Redis Protocol Engine (ADR 0005, Phase 2, P2-1).
 *
 * Two test suites in one binary:
 *
 *   1. RESP codec (resp.c) — encode/decode without any network.
 *   2. Redis vtable (redis.c) — loopback fixture, same pattern as test_http1.c:
 *      a real in-process TCP listener, bytes injected from the test side.
 *
 * No libluajit, no orchestrator linked. Proves the protocol is testable by
 * feeding it raw bytes (ADR 0001 Consequences).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "unity.h"
#include "proto/proto.h"
#include "proto/resp.h"
#include "proto/redis.h"

/* =========================================================================
 * Loopback fixture
 * ====================================================================== */

static int              listen_fd = -1;
static struct addrinfo *g_addr    = NULL;
static int              server_fd = -1;

static protocol   *redis_proto;
static connection  conn;

static struct addrinfo *resolve_loopback(uint16_t port) {
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("127.0.0.1", portstr, &hints, &res) != 0) return NULL;
    return res;
}

static void start_listener(void) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(listen_fd >= 0);

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)));
    TEST_ASSERT_EQUAL_INT(0, listen(listen_fd, 8));

    socklen_t slen = sizeof(sa);
    TEST_ASSERT_EQUAL_INT(0, getsockname(listen_fd, (struct sockaddr *)&sa, &slen));

    g_addr = resolve_loopback(ntohs(sa.sin_port));
    TEST_ASSERT_NOT_NULL(g_addr);

    redis_proto = redis_protocol();
    redis_configure(g_addr, NULL /*plain TCP*/, "127.0.0.1", NULL /*no AUTH*/, 0 /*no SELECT*/);
}

static void open_connection(void) {
    memset(&conn, 0, sizeof(conn));
    conn.fd = -1;
    TEST_ASSERT_EQUAL_INT(0, redis_proto->connect(&conn));
    TEST_ASSERT_TRUE(conn.fd >= 0);

    server_fd = -1;
    for (int i = 0; i < 1000 && server_fd < 0; i++) {
        server_fd = accept(listen_fd, NULL, NULL);
        if (server_fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        break;
    }
    TEST_ASSERT_TRUE(server_fd >= 0);
}

static void server_send(const char *bytes, size_t len) {
    ssize_t w = write(server_fd, bytes, len);
    TEST_ASSERT_EQUAL_INT((int)len, (int)w);
    usleep(2000);
}

static proto_status drive_readable(void) {
    return redis_proto->readable(&conn);
}

void setUp(void) {
    server_fd = -1;
    open_connection();
}

void tearDown(void) {
    redis_proto->close(&conn);
    if (server_fd >= 0) { close(server_fd); server_fd = -1; }
}

/* =========================================================================
 * RESP codec tests (no network)
 * ====================================================================== */

void test_resp_encode_simple(void) {
    /* SET key val -> *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n */
    const char *argv[]  = { "SET", "key", "val" };
    size_t      lens[]  = { 3, 3, 3 };
    char buf[128];
    int n = resp_encode(buf, sizeof(buf), 3, argv, lens);
    TEST_ASSERT_TRUE(n > 0);
    const char *expected = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, (size_t)n);
}

void test_resp_encode_buffer_too_small(void) {
    const char *argv[] = { "PING" };
    size_t      lens[] = { 4 };
    char buf[4]; /* way too small */
    int n = resp_encode(buf, sizeof(buf), 1, argv, lens);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_resp_parse_simple_string(void) {
    const char *r = "+OK\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_EQUAL_INT(5, rc);
    TEST_ASSERT_EQUAL_UINT(5, out);
}

void test_resp_parse_error_reply(void) {
    const char *r = "-ERR unknown command\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_TRUE(rc > 0);
    TEST_ASSERT_EQUAL_UINT(strlen(r), out);
}

void test_resp_parse_integer(void) {
    const char *r = ":42\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_EQUAL_INT(5, rc);
    TEST_ASSERT_EQUAL_UINT(5, out);
}

void test_resp_parse_bulk_string(void) {
    const char *r = "$6\r\nfoobar\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_EQUAL_INT(12, rc);
    TEST_ASSERT_EQUAL_UINT(12, out);
}

void test_resp_parse_nil_bulk(void) {
    const char *r = "$-1\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_TRUE(rc > 0);       /* nil bulk is a complete response */
    TEST_ASSERT_EQUAL_UINT(5, out);
}

void test_resp_parse_array(void) {
    /* *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n */
    const char *r = "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_TRUE(rc > 0);
    TEST_ASSERT_EQUAL_UINT(strlen(r), out);
}

void test_resp_parse_null_array(void) {
    const char *r = "*-1\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_TRUE(rc > 0);
    TEST_ASSERT_EQUAL_UINT(5, out);
}

void test_resp_parse_partial_returns_zero(void) {
    /* Truncated bulk string: only the header, no data. */
    const char *r = "$6\r\nfoo";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void test_resp_parse_split_delivery(void) {
    /* Deliver in two chunks and assert correct behaviour at each step. */
    const char full[] = "$6\r\nfoobar\r\n";
    size_t part = 8; /* "$6\r\nfoob" — not complete */
    size_t out = 0;

    int rc = resp_parse(full, part, &out);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = resp_parse(full, strlen(full), &out);
    TEST_ASSERT_EQUAL_INT(12, rc);
    TEST_ASSERT_EQUAL_UINT(12, out);
}

void test_resp_parse_unknown_type(void) {
    const char *r = "!something\r\n";
    size_t out = 0;
    int rc = resp_parse(r, strlen(r), &out);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* =========================================================================
 * Redis vtable tests (loopback)
 * ====================================================================== */

void test_redis_connect_no_auth(void) {
    /* setUp() already called connect(); just verify the fd is valid. */
    TEST_ASSERT_TRUE(conn.fd >= 0);
    TEST_ASSERT_NOT_NULL(conn.proto_state);
}

void test_redis_readable_ok_reply(void) {
    const char *r = "+OK\r\n";
    conn.bytes = 0;
    server_send(r, strlen(r));
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    TEST_ASSERT_EQUAL_UINT(5, (unsigned)conn.bytes);
}

void test_redis_readable_error_reply(void) {
    const char *r = "-ERR no such key\r\n";
    conn.bytes = 0;
    server_send(r, strlen(r));
    TEST_ASSERT_EQUAL_INT(PROTO_DONE_STATUS_ERR, drive_readable());
    TEST_ASSERT_EQUAL_UINT(strlen(r), (unsigned)conn.bytes);
}

void test_redis_readable_integer_reply(void) {
    const char *r = ":1000\r\n";
    conn.bytes = 0;
    server_send(r, strlen(r));
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    TEST_ASSERT_EQUAL_UINT(strlen(r), (unsigned)conn.bytes);
}

void test_redis_readable_bulk_string(void) {
    const char *r = "$6\r\nfoobar\r\n";
    conn.bytes = 0;
    server_send(r, strlen(r));
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    TEST_ASSERT_EQUAL_UINT(12, (unsigned)conn.bytes);
}

void test_redis_readable_partial_then_done(void) {
    /* First chunk: header only of a bulk string — not yet complete. */
    server_send("$6\r\nfoo", 7);
    TEST_ASSERT_EQUAL_INT(PROTO_PENDING, drive_readable());

    /* Second chunk: remaining data — now complete. */
    server_send("bar\r\n", 5);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    TEST_ASSERT_EQUAL_UINT(12, (unsigned)conn.bytes);
}

void test_redis_readable_peer_close_is_error(void) {
    close(server_fd);
    server_fd = -1;
    usleep(2000);
    TEST_ASSERT_EQUAL_INT(PROTO_ERROR, drive_readable());
}

void test_redis_two_responses_one_connection(void) {
    /* First response. */
    server_send("+OK\r\n", 5);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());

    /* Simulate a write() resetting state for the next request. */
    const char *ping = "*1\r\n$4\r\nPING\r\n";
    int wrc = redis_proto->write(&conn, ping, strlen(ping));
    TEST_ASSERT_TRUE(wrc >= 0);
    /* Drain whatever the server sees. */
    char drain[64];
    ssize_t dn = recv(server_fd, drain, sizeof(drain), MSG_DONTWAIT);
    (void)dn;

    /* Second response parses cleanly. */
    server_send("+PONG\r\n", 7);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    TEST_ASSERT_EQUAL_UINT(7, (unsigned)conn.bytes);
}

/* =========================================================================
 * Pipeline tests (depth > 1)
 * ====================================================================== */

/*
 * Helper: send N RESP-encoded commands as one write() call (simulating what
 * the orchestrator does when the Lua request() returns redis.pipeline(…)).
 * Returns the number of bytes accepted.
 */
static int pipeline_write(int n, const char *cmd_resp) {
    /* Build a buffer with n copies of cmd_resp concatenated. */
    size_t clen  = strlen(cmd_resp);
    size_t total = clen * (size_t)n;
    char  *buf   = malloc(total);
    TEST_ASSERT_NOT_NULL(buf);
    for (int i = 0; i < n; i++)
        memcpy(buf + i * clen, cmd_resp, clen);
    int rc = redis_proto->write(&conn, buf, total);
    free(buf);
    return rc;
}

void test_pipeline_depth2_both_ok(void) {
    /* Write 2 PING commands as a pipeline. */
    const char *ping = "*1\r\n$4\r\nPING\r\n";
    pipeline_write(2, ping);

    /* Drain the bytes the server received. */
    char drain[128];
    recv(server_fd, drain, sizeof(drain), MSG_DONTWAIT);

    /* Protocol expects 2 replies before returning PROTO_DONE. */
    /* Send first reply — should return PROTO_PENDING. */
    server_send("+PONG\r\n", 7);
    TEST_ASSERT_EQUAL_INT(PROTO_PENDING, drive_readable());

    /* Send second reply — should now return PROTO_DONE. */
    server_send("+PONG\r\n", 7);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    /* bytes should be sum of both replies: 7 + 7 = 14 */
    TEST_ASSERT_EQUAL_UINT(14, (unsigned)conn.bytes);
}

void test_pipeline_depth3_accumulated_bytes(void) {
    /* GET key → $5\r\nvalue\r\n (11 bytes each) */
    const char *get = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
    pipeline_write(3, get);
    char drain[256];
    recv(server_fd, drain, sizeof(drain), MSG_DONTWAIT);

    /* All 3 replies arrive in one server_send (simulating pipelining). */
    const char *three_replies = "$5\r\nvalue\r\n$5\r\nvalue\r\n$5\r\nvalue\r\n";
    server_send(three_replies, strlen(three_replies));

    /* May take multiple readable() calls if data arrives in chunks, but
     * in practice one recv() delivers all 33 bytes on loopback. */
    proto_status st = drive_readable();
    /* Could be PROTO_PENDING if data hasn't arrived yet — loop a bit. */
    for (int i = 0; i < 10 && st == PROTO_PENDING; i++) {
        usleep(2000);
        st = drive_readable();
    }
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, st);
    /* 3 × 11 bytes = 33  ($5\r\nvalue\r\n = 1+1+2+5+2 = 11) */
    TEST_ASSERT_EQUAL_UINT(33, (unsigned)conn.bytes);
}

void test_pipeline_depth2_error_in_second(void) {
    /* Two commands: first OK, second an error reply. */
    const char *ping = "*1\r\n$4\r\nPING\r\n";
    pipeline_write(2, ping);
    char drain[128];
    recv(server_fd, drain, sizeof(drain), MSG_DONTWAIT);

    server_send("+PONG\r\n", 7);
    TEST_ASSERT_EQUAL_INT(PROTO_PENDING, drive_readable());

    server_send("-ERR oops\r\n", 11);
    proto_status st = drive_readable();
    TEST_ASSERT_EQUAL_INT(PROTO_DONE_STATUS_ERR, st);
}

void test_pipeline_depth1_unchanged(void) {
    /* Depth 1 is the existing behaviour — single command, single reply. */
    const char *set = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    int wrc = redis_proto->write(&conn, set, strlen(set));
    TEST_ASSERT_TRUE(wrc >= 0);
    char drain[64];
    recv(server_fd, drain, sizeof(drain), MSG_DONTWAIT);

    server_send("+OK\r\n", 5);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
    TEST_ASSERT_EQUAL_UINT(5, (unsigned)conn.bytes);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    /* Start the listener once for the whole run; setUp/tearDown cycle per test. */
    start_listener();

    UNITY_BEGIN();

    /* RESP codec */
    RUN_TEST(test_resp_encode_simple);
    RUN_TEST(test_resp_encode_buffer_too_small);
    RUN_TEST(test_resp_parse_simple_string);
    RUN_TEST(test_resp_parse_error_reply);
    RUN_TEST(test_resp_parse_integer);
    RUN_TEST(test_resp_parse_bulk_string);
    RUN_TEST(test_resp_parse_nil_bulk);
    RUN_TEST(test_resp_parse_array);
    RUN_TEST(test_resp_parse_null_array);
    RUN_TEST(test_resp_parse_partial_returns_zero);
    RUN_TEST(test_resp_parse_split_delivery);
    RUN_TEST(test_resp_parse_unknown_type);

    /* Redis vtable */
    RUN_TEST(test_redis_connect_no_auth);
    RUN_TEST(test_redis_readable_ok_reply);
    RUN_TEST(test_redis_readable_error_reply);
    RUN_TEST(test_redis_readable_integer_reply);
    RUN_TEST(test_redis_readable_bulk_string);
    RUN_TEST(test_redis_readable_partial_then_done);
    RUN_TEST(test_redis_readable_peer_close_is_error);
    RUN_TEST(test_redis_two_responses_one_connection);

    /* Pipeline (depth > 1) */
    RUN_TEST(test_pipeline_depth1_unchanged);
    RUN_TEST(test_pipeline_depth2_both_ok);
    RUN_TEST(test_pipeline_depth3_accumulated_bytes);
    RUN_TEST(test_pipeline_depth2_error_in_second);

    int result = UNITY_END();

    if (g_addr) freeaddrinfo(g_addr);
    if (listen_fd >= 0) close(listen_fd);

    return result;
}
