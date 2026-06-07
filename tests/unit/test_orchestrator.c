/*
 * Unit test for the Orchestrator layer (ADR 0001, P1-2).
 *
 * Drives the real orchestrator with a STUB protocol and NO scripting engine
 * (NULL script_engine). No real network, no LuaJIT. The stub protocol uses a
 * socketpair per connection so the orchestrator's ae loop fires genuine
 * readable/writeable events: write() pokes the peer end, and readable()
 * drains it and reports PROTO_DONE — exactly the "is the response complete?"
 * question the orchestrator asks.
 *
 * This proves the Orchestrator is testable in isolation, per the ADR
 * Consequences ("the Orchestrator can be tested with a stub protocol").
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "unity.h"
#include "orchestrator.h"
#include "proto/proto.h"
#include "scripting/script_api.h"
#include "rate.h"
#include "hdr_histogram.h"

/* ------------------------------------------------------------------------- */
/* Stub protocol                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    int peer;   /* the far end of the socketpair                              */
} stub_state;

/* Count protocol-layer calls so the test can assert the orchestrator drives
 * the vtable. Single-threaded test (threads=1) so plain ints are fine. */
static int stub_connects = 0;
static int stub_writes   = 0;
static int stub_readables = 0;

static int stub_connect(connection *c) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(sv[i], F_GETFL, 0);
        fcntl(sv[i], F_SETFL, fl | O_NONBLOCK);
    }
    stub_state *s = calloc(1, sizeof(*s));
    if (!s) { close(sv[0]); close(sv[1]); return -1; }
    s->peer = sv[1];
    c->fd = sv[0];
    c->proto_state = s;
    stub_connects++;
    return 0;
}

static int stub_write(connection *c, const char *buf, size_t len) {
    stub_state *s = c->proto_state;
    (void)buf;
    /* Make the connection fd readable by sending one byte to its peer. */
    char b = 'x';
    ssize_t n = write(s->peer, &b, 1);
    (void)n;
    stub_writes++;
    return (int)len; /* pretend we accepted the whole request */
}

static proto_status stub_readable(connection *c) {
    stub_state *s = c->proto_state;
    char buf[64];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    (void)n; (void)s;
    stub_readables++;
    return PROTO_DONE; /* canned: a complete response has arrived */
}

static void stub_close(connection *c) {
    stub_state *s = c->proto_state;
    if (s) {
        if (s->peer >= 0) close(s->peer);
        free(s);
        c->proto_state = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static protocol stub_proto = {
    .name     = "stub",
    .connect  = stub_connect,
    .write    = stub_write,
    .readable = stub_readable,
    .close    = stub_close,
};

void setUp(void) {
    stub_connects = stub_writes = stub_readables = 0;
}

void tearDown(void) {}

/* ------------------------------------------------------------------------- */
/* Mock script_api for clone/init lifecycle tests                            */
/* ------------------------------------------------------------------------- */

/* Minimal heap object so distinct clone pointers are detectable. */
typedef struct { int id; } mock_engine;

static uint32_t mock_caps_value   = 0;
static int      mock_init_count   = 0;
static uint64_t mock_init_ids[8]; /* thread_ids passed to init(), in order  */
static int      mock_clone_count  = 0;

static script_engine *mock_create(const char *file) {
    (void)file;
    mock_engine *e = calloc(1, sizeof(*e));
    return (script_engine *) e;
}

static uint32_t mock_capabilities(script_engine *e) {
    (void)e;
    return mock_caps_value;
}

static script_engine *mock_clone(script_engine *src) {
    (void)src;
    mock_engine *e = calloc(1, sizeof(*e));
    e->id = ++mock_clone_count;
    return (script_engine *) e;
}

static void mock_init(script_engine *e, uint64_t thread_id, uint64_t conns) {
    (void)e; (void)conns;
    if (mock_init_count < 8)
        mock_init_ids[mock_init_count] = thread_id;
    mock_init_count++;
}

static char *mock_request(script_engine *e, size_t *len_out) {
    (void)e;
    char *buf = strdup("MOCK");
    if (len_out) *len_out = 4;
    return buf;
}

static void mock_destroy(script_engine *e) { free(e); }

static script_api mock_api_static = {
    .name         = "mock",
    .create       = mock_create,
    .capabilities = mock_capabilities,
    .clone        = mock_clone,
    .init         = mock_init,
    .request      = mock_request,
    .destroy      = mock_destroy,
};

static void reset_mock(void) {
    mock_caps_value  = 0;
    mock_init_count  = 0;
    mock_clone_count = 0;
    memset(mock_init_ids, 0, sizeof(mock_init_ids));
}

/* ------------------------------------------------------------------------- */
/* rate.c unit coverage (pure, no event loop)                                */
/* ------------------------------------------------------------------------- */

void test_rate_init_defaults(void) {
    rate_conn rc;
    rate_conn_init(&rc, 0.001, 1000);
    TEST_ASSERT_EQUAL_UINT64(1000, rc.thread_start);
    TEST_ASSERT_EQUAL_UINT64(0, rc.complete);
    TEST_ASSERT_TRUE(rc.caught_up);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.002, rc.catch_up_throughput);
}

void test_rate_send_now_when_behind(void) {
    rate_conn rc;
    /* thread started long ago, throughput high -> we are behind -> send now */
    rate_conn_init(&rc, 1.0, 0);
    uint64_t wait = rate_usec_to_next_send(&rc, 1000000);
    TEST_ASSERT_EQUAL_UINT64(0, wait);
}

void test_rate_wait_when_ahead(void) {
    rate_conn rc;
    uint64_t now = 1000000;
    rate_conn_init(&rc, 0.001, now); /* 1 req / 1000us */
    rc.complete = 10;                 /* next start = now + 10000us */
    uint64_t wait = rate_usec_to_next_send(&rc, now);
    TEST_ASSERT_TRUE(wait > 0);
}

void test_rate_expected_latency_nonnegative(void) {
    rate_conn rc;
    rate_conn_init(&rc, 0.001, 0);
    rate_begin_batch(&rc, 100);
    int64_t unc = -1;
    int64_t exp = rate_expected_latency(&rc, 200, &unc);
    TEST_ASSERT_TRUE(exp >= 0);
    TEST_ASSERT_TRUE(unc >= 0);
    TEST_ASSERT_EQUAL_UINT64(1, rc.complete);
}

void test_rate_calibrate_needs_data(void) {
    struct hdr_histogram *h;
    hdr_init(1, 1000000, 3, &h);
    uint64_t mean = 0; int interval = 0;
    TEST_ASSERT_FALSE(rate_calibrate(h, &mean, &interval)); /* empty -> false */
    for (int i = 0; i < 100; i++) hdr_record_value(h, 1000);
    TEST_ASSERT_TRUE(rate_calibrate(h, &mean, &interval));
    TEST_ASSERT_TRUE(mean > 0);
    TEST_ASSERT_TRUE(interval >= 10);
    free(h);
}

/* ------------------------------------------------------------------------- */
/* Orchestrator lifecycle with the stub protocol                             */
/* ------------------------------------------------------------------------- */

void test_create_destroy(void) {
    orchestrator_cfg cfg = {
        .connections = 2, .threads = 1, .duration_us = 0, .rate = 1000
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, NULL, NULL);
    TEST_ASSERT_NOT_NULL(o);
    orchestrator_destroy(o);
}

void test_create_null_proto_fails(void) {
    orchestrator_cfg cfg = { .connections = 1, .threads = 1, .rate = 1 };
    TEST_ASSERT_NULL(orchestrator_create(cfg, NULL, NULL, NULL));
}

void test_run_drives_protocol_and_collects(void) {
    orchestrator_cfg cfg = {
        .connections = 2,
        .threads     = 1,
        .duration_us = 200000,   /* 200 ms */
        .rate        = 100000,   /* high rate so we are always "behind"      */
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, NULL, NULL);
    TEST_ASSERT_NOT_NULL(o);

    int rc = orchestrator_run(o);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* The orchestrator must have driven the protocol vtable. */
    TEST_ASSERT_TRUE(stub_connects >= 1);
    TEST_ASSERT_TRUE(stub_writes   >= 1);
    TEST_ASSERT_TRUE(stub_readables >= 1);

    orchestrator_stats st = orchestrator_collect(o);
    TEST_ASSERT_NOT_NULL(st.latency);
    TEST_ASSERT_TRUE(st.requests >= 1);
    TEST_ASSERT_TRUE(st.elapsed_us > 0);
    TEST_ASSERT_TRUE(st.start_us > 0);

    orchestrator_destroy(o);
}

void test_collect_null_safe(void) {
    orchestrator_stats st = orchestrator_collect(NULL);
    TEST_ASSERT_NULL(st.latency);
    TEST_ASSERT_EQUAL_UINT64(0, st.requests);
}

/*
 * Regression test for the event-loop-size bug (t036):
 *
 * Each per-thread ae event loop must be sized for the TOTAL connection count,
 * not the per-thread count.  File descriptors are process-wide; later threads
 * get higher fd numbers.  If aeCreateEventLoop(10 + per_thread*3) is used,
 * thread N's fds exceed setsize and aeCreateFileEvent returns AE_ERR, causing
 * oc_connect to count them as connect errors and leave those connections dead
 * for the whole run.  With 32 connections across 4 threads (8 per thread),
 * the old formula gave setsize=34; thread 3's fds (3 event-loop fds + 24
 * socketpair fds = ~51+) all exceed 33, producing 7+ connect errors and
 * ~22% throughput shortfall.
 *
 * The fix: use o->cfg.connections (total) in aeCreateEventLoop, giving
 * setsize=106 — matching the legacy wrk.c formula.
 */
void test_all_connections_established_multi_thread(void) {
    /* 4 threads × 8 connections = 32 total; matches the user's -c32 -t4 case */
    orchestrator_cfg cfg = {
        .connections = 32,
        .threads     = 4,
        .duration_us = 300000,   /* 300 ms — long enough to observe all connects */
        .rate        = 100000,   /* high rate so connections are exercised quickly */
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, NULL, NULL);
    TEST_ASSERT_NOT_NULL(o);

    orchestrator_run(o);

    /* Every connection must have connected exactly once at startup. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        (int)cfg.connections, stub_connects,
        "not all connections established — event loop setsize too small?");

    /* No connect errors: oc_connect must not have failed for any fd. */
    orchestrator_stats st = orchestrator_collect(o);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        0, st.errors_connect,
        "connect errors detected — event loop setsize too small?");

    orchestrator_destroy(o);
}

/* ------------------------------------------------------------------------- */
/* Clone / init lifecycle tests (ADR 0005 Phase 5 t070)                     */
/* ------------------------------------------------------------------------- */

void test_dynamic_off_for_static_script(void) {
    /* caps = 0 → static workload; all threads share template, dynamic==false. */
    reset_mock();
    mock_caps_value = 0;

    script_engine *tmpl = mock_create(NULL);
    orchestrator_cfg cfg = {
        .connections = 4, .threads = 2, .duration_us = 100000, .rate = 100000,
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, &mock_api_static, tmpl);
    TEST_ASSERT_NOT_NULL(o);

    orchestrator_run(o);

    /* No clones: all threads point at the template. */
    TEST_ASSERT_EQUAL_INT(0, mock_clone_count);
    orchestrator_destroy(o);
    mock_destroy(tmpl);
}

void test_dynamic_on_clones_per_thread(void) {
    /* caps = DYNAMIC_REQUEST → threads 1..N each get a distinct engine. */
    reset_mock();
    mock_caps_value = SCRIPT_CAP_DYNAMIC_REQUEST;

    script_engine *tmpl = mock_create(NULL);
    orchestrator_cfg cfg = {
        .connections = 4, .threads = 3, .duration_us = 100000, .rate = 100000,
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, &mock_api_static, tmpl);
    TEST_ASSERT_NOT_NULL(o);

    /* 3 threads → thread 0 = template, threads 1 and 2 get clones. */
    TEST_ASSERT_EQUAL_INT(2, mock_clone_count);

    orchestrator_run(o);
    orchestrator_destroy(o);
    mock_destroy(tmpl);
}

void test_template_init_runs_once(void) {
    /* Static workload: init() called exactly once, on the template, thread_id 0. */
    reset_mock();
    mock_caps_value = 0;

    script_engine *tmpl = mock_create(NULL);
    orchestrator_cfg cfg = {
        .connections = 2, .threads = 2, .duration_us = 100000, .rate = 100000,
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, &mock_api_static, tmpl);
    TEST_ASSERT_NOT_NULL(o);

    orchestrator_run(o);

    TEST_ASSERT_EQUAL_INT(1, mock_init_count);
    TEST_ASSERT_EQUAL_UINT64(0, mock_init_ids[0]);

    orchestrator_destroy(o);
    mock_destroy(tmpl);
}

void test_clone_init_per_thread(void) {
    /* Dynamic workload: template init'd with thread_id 0; each clone init'd
     * with its own thread_id; template never re-init'd. */
    reset_mock();
    mock_caps_value = SCRIPT_CAP_DYNAMIC_REQUEST;

    script_engine *tmpl = mock_create(NULL);
    orchestrator_cfg cfg = {
        .connections = 3, .threads = 3, .duration_us = 100000, .rate = 100000,
    };
    orchestrator *o = orchestrator_create(cfg, &stub_proto, &mock_api_static, tmpl);
    TEST_ASSERT_NOT_NULL(o);
    /* 2 clones created at orchestrator_create time. */
    TEST_ASSERT_EQUAL_INT(2, mock_clone_count);

    orchestrator_run(o);

    /* init() must have been called 3 times: once for template (id=0), once per
     * clone (id=1, id=2). */
    TEST_ASSERT_EQUAL_INT(3, mock_init_count);
    TEST_ASSERT_EQUAL_UINT64(0, mock_init_ids[0]);
    /* ids 1 and 2 in some order for threads 1 and 2 */
    TEST_ASSERT_TRUE(mock_init_ids[1] == 1 || mock_init_ids[1] == 2);
    TEST_ASSERT_TRUE(mock_init_ids[2] == 1 || mock_init_ids[2] == 2);
    TEST_ASSERT_NOT_EQUAL(mock_init_ids[1], mock_init_ids[2]);

    orchestrator_destroy(o);
    mock_destroy(tmpl);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rate_init_defaults);
    RUN_TEST(test_rate_send_now_when_behind);
    RUN_TEST(test_rate_wait_when_ahead);
    RUN_TEST(test_rate_expected_latency_nonnegative);
    RUN_TEST(test_rate_calibrate_needs_data);
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_create_null_proto_fails);
    RUN_TEST(test_run_drives_protocol_and_collects);
    RUN_TEST(test_collect_null_safe);
    RUN_TEST(test_all_connections_established_multi_thread);

    RUN_TEST(test_dynamic_off_for_static_script);
    RUN_TEST(test_dynamic_on_clones_per_thread);
    RUN_TEST(test_template_init_runs_once);
    RUN_TEST(test_clone_init_per_thread);

    return UNITY_END();
}
