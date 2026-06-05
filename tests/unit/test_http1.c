/*
 * Unit test for the HTTP/1.1 Protocol Engine (ADR 0001, P1-3).
 *
 * Feeds RAW BYTES directly into the http1 `readable` path with no running
 * engine and no external network. A loopback TCP listener stands in for the
 * server: the test writes hand-crafted response bytes into the peer end of an
 * in-process connection and asserts the tri-state proto_status:
 *
 *   - partial response  -> PROTO_PENDING
 *   - complete response  -> PROTO_DONE
 *   - malformed bytes    -> PROTO_ERROR
 *
 * This proves a protocol is testable by feeding it raw bytes (ADR Consequences:
 * "a protocol can be tested by feeding it raw bytes without a running engine").
 * The target binary links NO libluajit and NO orchestrator.
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
#include "proto/http1.h"

/* ------------------------------------------------------------------------- */
/* Loopback fixture: a real in-process TCP server the http1 connect() dials. */
/* ------------------------------------------------------------------------- */

static int              listen_fd = -1;
static struct addrinfo *g_addr    = NULL;
static int              server_fd = -1;  /* accepted peer (we write bytes here)*/

static protocol   *http1;
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

/* Start a listener on an ephemeral loopback port and configure http1 to it. */
static void start_listener(void) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(listen_fd >= 0);

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0; /* ephemeral */
    TEST_ASSERT_EQUAL_INT(0, bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)));
    TEST_ASSERT_EQUAL_INT(0, listen(listen_fd, 4));

    socklen_t slen = sizeof(sa);
    TEST_ASSERT_EQUAL_INT(0, getsockname(listen_fd, (struct sockaddr *)&sa, &slen));

    g_addr = resolve_loopback(ntohs(sa.sin_port));
    TEST_ASSERT_NOT_NULL(g_addr);

    http1 = http1_protocol();
    http1_configure(g_addr, NULL, "127.0.0.1");
}

/* Connect via the vtable and accept the peer so we can inject response bytes. */
static void open_connection(void) {
    memset(&conn, 0, sizeof(conn));
    conn.fd = -1;
    TEST_ASSERT_EQUAL_INT(0, http1->connect(&conn));
    TEST_ASSERT_TRUE(conn.fd >= 0);

    /* Accept the non-blocking connect; retry briefly until it lands. */
    server_fd = -1;
    for (int i = 0; i < 1000 && server_fd < 0; i++) {
        server_fd = accept(listen_fd, NULL, NULL);
        if (server_fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        break;
    }
    /* Listener is blocking, so accept should succeed promptly. */
    TEST_ASSERT_TRUE(server_fd >= 0);
}

/* Push raw bytes to the connection (as if the server sent them). */
static void server_send(const char *bytes, size_t len) {
    ssize_t w = write(server_fd, bytes, len);
    TEST_ASSERT_EQUAL_INT((int)len, (int)w);
    /* Give the bytes time to traverse the loopback before readable(). */
    usleep(2000);
}

/* Run readable once and return the tri-state. */
static proto_status drive_readable(void) {
    return http1->readable(&conn);
}

void setUp(void) {
    server_fd = -1;
    open_connection();
}

void tearDown(void) {
    http1->close(&conn);
    if (server_fd >= 0) { close(server_fd); server_fd = -1; }
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static const char COMPLETE_RESP[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "hello";

void test_complete_response_is_done(void) {
    server_send(COMPLETE_RESP, sizeof(COMPLETE_RESP) - 1);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
}

void test_partial_headers_are_pending(void) {
    /* Status line + a header, but no blank line terminating the headers. */
    const char *partial =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n";
    server_send(partial, strlen(partial));
    TEST_ASSERT_EQUAL_INT(PROTO_PENDING, drive_readable());
}

void test_partial_body_is_pending(void) {
    /* Full headers promising 10 bytes but only 3 delivered. */
    const char *partial =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "abc";
    server_send(partial, strlen(partial));
    TEST_ASSERT_EQUAL_INT(PROTO_PENDING, drive_readable());
}

void test_pending_then_done_across_reads(void) {
    /* First chunk: headers only -> PENDING. */
    const char *part1 =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "ab";
    server_send(part1, strlen(part1));
    TEST_ASSERT_EQUAL_INT(PROTO_PENDING, drive_readable());

    /* Second chunk completes the body -> DONE. */
    const char *part2 = "cde";
    server_send(part2, strlen(part2));
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
}

void test_malformed_response_is_error(void) {
    /* Garbage that is not a valid HTTP status line. */
    const char *junk = "NOT-HTTP \r\n garbage \r\n\r\n";
    server_send(junk, strlen(junk));
    TEST_ASSERT_EQUAL_INT(PROTO_ERROR, drive_readable());
}

void test_peer_close_without_response_is_error(void) {
    /* Server closes without sending a complete (or any) response. */
    close(server_fd);
    server_fd = -1;
    /* Allow the FIN to arrive. */
    usleep(2000);
    TEST_ASSERT_EQUAL_INT(PROTO_ERROR, drive_readable());
}

void test_two_responses_on_one_connection(void) {
    /* First full response. */
    server_send(COMPLETE_RESP, sizeof(COMPLETE_RESP) - 1);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());

    /* A write() resets the parser for the next request/response cycle. */
    int rc = http1->write(&conn, "GET / HTTP/1.1\r\n\r\n", 18);
    TEST_ASSERT_TRUE(rc >= 0);
    /* Drain whatever the server received (we don't assert on it). */
    char drain[64];
    ssize_t dn = recv(server_fd, drain, sizeof(drain), MSG_DONTWAIT);
    (void)dn;

    /* Second full response parses cleanly after the reset. */
    server_send(COMPLETE_RESP, sizeof(COMPLETE_RESP) - 1);
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());
}

/*
 * Regression test for the phantom-completion flood (t036).
 *
 * On a Connection: close server the response is immediately followed by a
 * FIN. The orchestrator keeps the readable event armed, so readable() fires
 * again on the EOF. Before the fix, http1_readable left s->complete set after
 * reporting PROTO_DONE, so this second call re-reported PROTO_DONE for the SAME
 * response — a phantom completion. The level-triggered EOF then re-entered in a
 * tight loop, double-counting requests and corrupting rate pacing (request
 * count explodes, throughput runs ~10x over -R, latency balloons to seconds).
 *
 * Correct behaviour: the completion is reported exactly once; the subsequent
 * readable on the closed peer returns PROTO_ERROR so the orchestrator
 * reconnects (matching wrk.c, which reconnects on !keep_alive).
 */
void test_completion_then_close_reports_done_once(void) {
    /* Full response, then the server closes the connection. */
    server_send(COMPLETE_RESP, sizeof(COMPLETE_RESP) - 1);
    close(server_fd);
    server_fd = -1;
    usleep(2000);   /* let the response bytes + FIN arrive together */

    /* First readable: the one real completion. */
    TEST_ASSERT_EQUAL_INT(PROTO_DONE, drive_readable());

    /* Second readable: must NOT re-report the same response. The peer has
     * closed, so this is an error (reconnect), never a phantom PROTO_DONE. */
    proto_status second = drive_readable();
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
        PROTO_DONE, second,
        "phantom completion: same response reported twice");
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
        PROTO_DONE_STATUS_ERR, second,
        "phantom completion: same response reported twice");
}

int main(void) {
    start_listener();

    UNITY_BEGIN();
    RUN_TEST(test_complete_response_is_done);
    RUN_TEST(test_partial_headers_are_pending);
    RUN_TEST(test_partial_body_is_pending);
    RUN_TEST(test_pending_then_done_across_reads);
    RUN_TEST(test_malformed_response_is_error);
    RUN_TEST(test_peer_close_without_response_is_error);
    RUN_TEST(test_two_responses_on_one_connection);
    RUN_TEST(test_completion_then_close_reports_done_once);
    int rc = UNITY_END();

    if (g_addr) freeaddrinfo(g_addr);
    if (listen_fd >= 0) close(listen_fd);
    return rc;
}
