/*
 * Orchestrator layer ("Tank") implementation (ADR 0001, Phase 1, step P1-2).
 *
 * Owns the thread pool, the per-thread ae event loop, the connection pool, the
 * rate controller (rate.c), stats aggregation and the run lifecycle
 * (init -> connect -> run -> drain -> report).
 *
 * It asks the Protocol Engine one question via the `protocol` vtable
 * ("is the response complete?") and the Request Layer one question via
 * `script_api` ("what bytes should I send next?"). It contains no protocol or
 * scripting specifics.
 *
 * Invariant 1: this file must not #include any protocol or scripting header
 * other than proto.h and script_api.h. No engine or parser headers appear.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>

#include "orchestrator.h"
#include "proto/proto.h"
#include "scripting/script_api.h"

#include "rate.h"
#include "utils.h"
#include "stats.h"
#include "units.h"
#include "ae.h"
#include "hdr_histogram.h"

/* Max recordable latency of 1 day (matches wrk.c). */
#define MAX_LATENCY         (24L * 60 * 60 * 1000000)
#define CALIBRATE_DELAY_MS       10000
#define TIMEOUT_INTERVAL_MS      2000
#define DEFAULT_TIMEOUT_MS       2000
#define INITIAL_CONNECT_RETRY_MS 100

/* ------------------------------------------------------------------------- */
/* Internal types                                                            */
/* ------------------------------------------------------------------------- */

struct othread;

/*
 * Per-connection orchestrator state.
 *
 * NOTE on the frozen contract: `struct connection` (proto.h) may not gain
 * fields. So orchestrator-private per-connection state — the thread
 * back-pointer, the rate/CO timing, and the send cursor — lives here. The
 * shared `connection` is the FIRST member, so a protocol callback that
 * receives `connection *` could be recovered to the owning `oconn *` by a
 * container cast if ever needed. In practice the orchestrator always holds the
 * `oconn *` itself (it is the ae clientData) and only ever hands `&c->conn`
 * down to the protocol vtable. proto_state / script_state in `connection`
 * remain owned by their respective layers; the orchestrator never touches
 * their internals.
 */
typedef struct oconn {
    connection       conn;       /* MUST be first member (container relation) */
    struct othread  *thread;     /* back-pointer (orchestrator-private)       */
    rate_conn        rate;       /* rate / Coordinated-Omission state         */

    const char      *request;    /* current request bytes (owned by script)  */
    size_t           length;     /* total request length                     */
    size_t           written;    /* bytes written so far this request        */

    uint64_t         start;      /* timestamp of last send (timeout watchdog) */
    bool             in_flight;  /* a request is currently outstanding        */
    bool             initial_connect_error_counted; /* true once errors.connect charged for this conn */
    bool             initial_response_done; /* true after first response received; triggers
                                               signal_connection_ready so the measurement
                                               window never opens before all first-requests
                                               have completed */
} oconn;

typedef struct othread {
    pthread_t             thread;
    aeEventLoop          *loop;
    orchestrator         *orch;     /* back-pointer to the handle             */
    uint64_t              id;
    uint64_t              connections;
    double                throughput;    /* per-connection req/usec           */
    int                   interval;       /* rate sampling interval (ms)       */
    uint64_t              sample_start;
    uint64_t              requests;        /* requests since last sample        */
    uint64_t              complete;        /* completed inside measurement window */
    uint64_t              bytes;           /* bytes inside measurement window   */
    uint64_t              ramp_complete;   /* completed during connection ramp-up */
    uint64_t              ramp_bytes;      /* bytes during ramp-up              */
    errors                errors;
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *u_latency_histogram;
    script_engine        *engine;          /* this thread's engine instance     */
    oconn                *cs;
    bool                  dynamic;          /* re-ask script per request?        */
    bool                  wants_response;   /* deliver response() callbacks?      */
    const char           *static_request;   /* cached static request bytes       */
    size_t                static_length;
    char                  cal_msg[128];
} othread;

struct orchestrator {
    orchestrator_cfg cfg;
    protocol        *proto;
    script_engine   *engine;     /* template engine (thread 0 reuses)         */
    const script_api *api;       /* derived from engine (may be NULL in tests)*/

    othread         *threads;
    uint64_t         n_threads;

    /* Aggregate request-rate samples across threads (RPS distribution). */
    stats           *rps;
    pthread_mutex_t  rps_mutex;

    /* Aggregate latency. */
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *u_latency_histogram;

    orchestrator_stats result;

    /* Calibration / progress / drain state (formerly globals). */
    volatile int            calibrated_threads;
    volatile int            workers_ready;   /* workers that have entered aeMain */
    volatile sig_atomic_t   stop;
    uint64_t                start_us;
    bool                    record_all_responses;
    uint64_t                timeout_ms;

    /* Connection-ready barrier: counting terminally-resolved initial connects.
     * total_connections is the number actually opened (per_thread * n_threads),
     * which may be less than cfg.connections if not evenly divisible. */
    uint64_t                total_connections;
    volatile int            connections_established;
    volatile uint64_t       connections_ready_at; /* 0 until all conns are up  */
    volatile uint64_t       stop_at;              /* shifted after conns ready  */
    uint64_t                workers_start_us;     /* when all workers entered aeMain */

    /* t049-fix: static request bytes generated once in the main thread before
     * workers start. All threads share the same read-only buffer.
     * Generating it per-thread via api->request() is a data race: multiple
     * threads calling lua_request() concurrently on the same lua_State →
     * LuaJIT is not thread-safe → segfault at calibration startup. */
    char                   *static_request_buf;
    size_t                  static_request_len;
};

/*
 * The signal-driven drain needs a process-wide handle: signal handlers cannot
 * carry a context argument. Only one orchestrator runs per process (one CLI
 * invocation), so a single static back-pointer is sufficient and matches the
 * old global `stop` flag.
 */
static orchestrator *g_active = NULL;

static void on_sigint(int sig) {
    (void)sig;
    if (g_active) g_active->stop = 1;
}

/* ------------------------------------------------------------------------- */
/* Script-API access helpers (NULL-safe for the stub-protocol unit test)     */
/* ------------------------------------------------------------------------- */

static const script_api *engine_api(orchestrator *o) {
    return o->api;
}

static void ask_request(othread *t, oconn *c) {
    orchestrator *o = t->orch;
    const script_api *api = engine_api(o);

    if (t->dynamic && api && api->request && t->engine) {
        size_t len = 0;
        char *buf = api->request(t->engine, &len);
        c->request = buf;
        c->length  = len;
    } else {
        c->request = t->static_request;
        c->length  = t->static_length;
    }
}

static void tell_response(othread *t, int status, size_t bytes,
                          uint64_t latency_us) {
    const script_api *api = engine_api(t->orch);
    if (t->wants_response && api && api->response && t->engine)
        api->response(t->engine, status, bytes, latency_us);
}

/* ------------------------------------------------------------------------- */
/* ae glue — thin delegation to the protocol vtable                          */
/* ------------------------------------------------------------------------- */

static int  oc_connect(othread *t, oconn *c);
static int  oc_reconnect(othread *t, oconn *c);
static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask);
static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask);

/* Called once per connection slot when its initial-connect attempt is
 * terminally resolved (either success or permanent failure / abandon).
 * When the last slot resolves, we record the ready timestamp so the main
 * thread can shift stop_at and start the measurement window. */
static void signal_connection_ready(othread *t) {
    orchestrator *o = t->orch;
    if (__sync_add_and_fetch(&o->connections_established, 1) != (int)o->total_connections)
        return;
    uint64_t zero = 0, now = time_us();
    __atomic_compare_exchange_n((uint64_t *)&o->connections_ready_at, &zero, now,
                                false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static int delayed_initial_connect(aeEventLoop *loop, long long id, void *data) {
    (void)loop; (void)id;
    oconn    *c = data;
    othread  *t = c->thread;

    if (!c->initial_connect_error_counted)
        c->rate.thread_start = time_us(); /* set once: preserves pacing/latency baseline */

    if (oc_connect(t, c) != -1) {
        /* Success. If this connection previously failed, count the recovery. */
        if (c->initial_connect_error_counted)
            t->errors.connect_recovered++;
        /* Do NOT signal here. signal_connection_ready fires after the first
         * response for this connection (in record_response), so connections_ready_at
         * is only set once every connection slot has completed its initial
         * request/response cycle. That prevents first-requests from leaking into
         * the measurement window when a connection establishes right as the last
         * slot is resolved. */
        return AE_NOMORE;
    }

    int e = errno;

    if (!c->initial_connect_error_counted) {
        /* First failure: errors.connect already charged by oc_connect. */
        c->initial_connect_error_counted = true;
    } else {
        /* Retry failure: undo the duplicate increment from oc_connect. */
        t->errors.connect--;
    }

    /* Retry selected TCP-level rejections; resource/ae failures are permanent. */
    if (e != ECONNREFUSED && e != ETIMEDOUT &&
        e != ENETUNREACH  && e != EHOSTUNREACH && e != ECONNRESET) {
        t->errors.connect_abandoned++;
        signal_connection_ready(t);
        return AE_NOMORE;
    }

    return INITIAL_CONNECT_RETRY_MS;
}

static int oc_connect(othread *t, oconn *c) {
    protocol *p = t->orch->proto;
    c->written  = 0;
    c->in_flight = false;

    if (p->connect(&c->conn) != 0) {
        t->errors.connect++;
        return -1;
    }

    int fd = c->conn.fd;
    if (aeCreateFileEvent(t->loop, fd, AE_READABLE, socket_readable, c) != AE_OK ||
        aeCreateFileEvent(t->loop, fd, AE_WRITABLE, socket_writeable, c) != AE_OK) {
        t->errors.connect++;
        p->close(&c->conn);
        return -1;
    }
    return fd;
}

static int oc_reconnect(othread *t, oconn *c) {
    /* If a transport error occurs before the first response arrives, this
     * connection slot will never call record_response with initial_response_done
     * false. Signal ready now so connections_ready_at is not blocked forever. */
    if (!c->initial_response_done) {
        c->initial_response_done = true;
        signal_connection_ready(t);
    }
    protocol *p = t->orch->proto;
    if (c->conn.fd >= 0)
        aeDeleteFileEvent(t->loop, c->conn.fd, AE_WRITABLE | AE_READABLE);
    p->close(&c->conn);
    return oc_connect(t, c);
}

static int delay_request(aeEventLoop *loop, long long id, void *data) {
    (void)loop; (void)id;
    oconn *c = data;
    uint64_t wait = rate_usec_to_next_send(&c->rate, time_us());
    if (wait) {
        return (int)round((wait / 1000.0L) + 0.5); /* still too early */
    }
    aeCreateFileEvent(c->thread->loop, c->conn.fd, AE_WRITABLE,
                      socket_writeable, c);
    return AE_NOMORE;
}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    (void)mask;
    oconn   *c = data;
    othread *t = c->thread;
    protocol *p = t->orch->proto;

    if (!c->written) {
        uint64_t now  = time_us();
        uint64_t wait = rate_usec_to_next_send(&c->rate, now);
        if (wait) {
            int msec = (int)round((wait / 1000.0L) + 0.5);
            aeDeleteFileEvent(loop, fd, AE_WRITABLE);
            aeCreateTimeEvent(t->loop, msec, delay_request, c, NULL);
            return;
        }
        /* First byte of a new request: pull bytes from the Request Layer. */
        ask_request(t, c);
        c->start = now;
        if (!c->in_flight) {
            rate_begin_batch(&c->rate, now);
            c->in_flight = true;
        }
    }

    /* Hand the Request Layer's bytes to the protocol. When there is no Request
     * Layer (length == 0) we still call write once with a zero-length buffer:
     * a protocol may synthesise its own request (e.g. log replay) or, like the
     * stub protocol in the unit test, drive the exchange itself. */
    const char *buf = c->request ? c->request + c->written : NULL;
    size_t      len = c->length  - c->written;

    int rc = p->write(&c->conn, buf, len);
    if (rc < 0) {
        t->errors.write++;
        oc_reconnect(t, c);
        return;
    }
    /* rc >= 0: number of bytes accepted this call. 0 means "retry later". */
    c->written += (size_t)rc;
    if (c->written >= c->length) {
        c->written = 0;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }
}

/*
 * Record one completed response: count it, record corrected/uncorrected
 * latency, notify the Request Layer. Returns true if the run should continue,
 * false if it should stop (now past stop_at, or a drain was requested) — in
 * which case the caller must not re-arm or reconnect.
 */
static bool record_response(othread *t, oconn *c) {
    orchestrator *o = t->orch;
    uint64_t now = time_us();

    /* Gate all measurement to the window [connections_ready_at, stop_at).
     * The rate-controller state (rc->complete, in_flight) must update
     * unconditionally so pacing stays correct outside the window. */
    bool measuring = (__atomic_load_n((uint64_t *)&o->connections_ready_at,
                                      __ATOMIC_RELAXED) != 0)
                     && !o->stop
                     && (now < __atomic_load_n((uint64_t *)&o->stop_at,
                                               __ATOMIC_RELAXED));
    if (measuring) {
        t->complete++;
        t->requests++;
        t->bytes += c->conn.bytes;
    } else if (__atomic_load_n((uint64_t *)&o->connections_ready_at,
                               __ATOMIC_RELAXED) == 0) {
        /* Before connections_ready_at: ramp-up phase. Count separately for
         * transparency in the "Total" output line. */
        t->ramp_complete++;
        t->ramp_bytes += c->conn.bytes;
    }

    int64_t uncorrected = 0;
    int64_t expected = rate_expected_latency(&c->rate, now, &uncorrected);

    c->in_flight = false;

    /* First response for this connection: signal the slot as resolved.
     * connections_ready_at is set when all slots fire this, which guarantees
     * the measurement window only opens after every connection has completed
     * its initial request/response cycle. */
    if (!c->initial_response_done) {
        c->initial_response_done = true;
        signal_connection_ready(t);
    }

    /* The orchestrator core sends one request per batch (pipelining is a
     * protocol concern handled inside proto->readable), so every completion
     * corresponds to one recordable latency sample. record_all_responses is
     * retained for parity with the -B batch-latency option once pipelining is
     * reintroduced at the protocol layer. */
    (void)o->record_all_responses;
    if (measuring) {
        hdr_record_value(t->latency_histogram, expected);
        hdr_record_value(t->u_latency_histogram, uncorrected);
    }

    /* Tell the Request Layer a response completed (status is protocol-defined;
     * the orchestrator does not interpret it). */
    tell_response(t, 0, 0, (uint64_t)expected);

    if (now >= o->stop_at || o->stop) {
        aeStop(t->loop);
        return false;
    }
    return true;
}

/* Completion on a kept-alive connection: record, then re-arm the writer to
 * send the next request on the same socket. */
static void complete_response(othread *t, oconn *c) {
    if (record_response(t, c))
        aeCreateFileEvent(t->loop, c->conn.fd, AE_WRITABLE, socket_writeable, c);
}

/*
 * Completion where the peer signalled close (ADR 0003-B). Record the response,
 * then reconnect cleanly — this is an expected, graceful close (e.g. nginx
 * keepalive_requests limit), NOT a transport error, so errors.read is NOT
 * incremented. Mirrors phase-0 wrk.c's reconnect-on-!keep_alive.
 */
static void complete_response_close(othread *t, oconn *c) {
    if (record_response(t, c))
        oc_reconnect(t, c);
}

static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    (void)loop; (void)fd; (void)mask;
    oconn   *c = data;
    othread *t = c->thread;
    protocol *p = t->orch->proto;

    proto_status st = p->readable(&c->conn);
    switch (st) {
        case PROTO_PENDING:
            return;
        case PROTO_DONE_STATUS_ERR:
            t->errors.status++;
            /* fall through: count as complete + record latency */
        case PROTO_DONE:
            complete_response(t, c);
            return;
        case PROTO_DONE_CLOSE:
            /* Graceful server close after a successful response: record and
             * reconnect cleanly, without counting a read error. */
            complete_response_close(t, c);
            return;
        case PROTO_ERROR:
        default:
            t->errors.read++;
            oc_reconnect(t, c);
            return;
    }
}

/* ------------------------------------------------------------------------- */
/* Rate-controller timed events                                              */
/* ------------------------------------------------------------------------- */

static int sample_rate(aeEventLoop *loop, long long id, void *data) {
    (void)loop; (void)id;
    othread *t = data;
    orchestrator *o = t->orch;

    uint64_t elapsed_ms = (time_us() - t->sample_start) / 1000;
    if (elapsed_ms == 0) elapsed_ms = 1;
    uint64_t requests = (uint64_t)((t->requests / (double)elapsed_ms) * 1000);

    pthread_mutex_lock(&o->rps_mutex);
    stats_record(o->rps, requests);
    pthread_mutex_unlock(&o->rps_mutex);

    t->requests     = 0;
    t->sample_start = time_us();
    return t->interval;
}

static int calibrate(aeEventLoop *loop, long long id, void *data) {
    (void)id;
    othread *t = data;

    uint64_t mean = 0;
    int interval = 0;
    if (!rate_calibrate(t->latency_histogram, &mean, &interval))
        return CALIBRATE_DELAY_MS; /* not enough data yet, retry */

    /* Do NOT reset the histograms. Counting is already gated on
     * connections_ready_at, so there are no ramp-up samples to discard.
     * Resetting here would drop valid post-ready measurements and create an
     * inconsistency between the histogram window and the Req/Sec window. */

    t->interval     = interval;
    t->sample_start = time_us();
    t->requests     = 0;

    snprintf(t->cal_msg, sizeof(t->cal_msg),
             "Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms",
             mean / 1000.0, interval);
    __sync_fetch_and_add(&t->orch->calibrated_threads, 1);

    aeCreateTimeEvent(loop, t->interval, sample_rate, t, NULL);
    return AE_NOMORE;
}

static int check_timeouts(aeEventLoop *loop, long long id, void *data) {
    (void)id;
    othread *t = data;
    orchestrator *o = t->orch;
    uint64_t now = time_us();
    uint64_t maxAge = now - (o->timeout_ms * 1000);

    for (uint64_t i = 0; i < t->connections; i++) {
        oconn *c = &t->cs[i];
        if (c->in_flight && maxAge > c->start)
            t->errors.timeout++;
    }

    if (o->stop || now >= o->stop_at) {
        aeStop(loop);
    }
    return TIMEOUT_INTERVAL_MS;
}

/* ------------------------------------------------------------------------- */
/* Thread main                                                               */
/* ------------------------------------------------------------------------- */

static void *thread_main(void *arg) {
    othread *t = arg;
    orchestrator *o = t->orch;

    hdr_init(1, MAX_LATENCY, 3, &t->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &t->u_latency_histogram);

    uint64_t now = time_us();

    /* Static request bytes are pre-generated in the main thread (orchestrator_run)
     * before workers are spawned and written to o->static_request_buf. Copy the
     * pointer here — no Lua call needed and no race on the shared lua_State. */
    if (!t->dynamic) {
        t->static_request = o->static_request_buf;
        t->static_length  = o->static_request_len;
    }

    /* Per-clone init (dynamic mode only). Thread 0 reuses the template engine
     * which was already init'd in orchestrator_run(); clones get their own init
     * with the real thread_id. Never re-init the template (t->engine == o->engine
     * for thread 0 and for any clone-fallback). */
    if (t->dynamic && t->engine != o->engine) {
        const script_api *api = engine_api(o);
        if (api && api->init && t->engine)
            api->init(t->engine, t->id, t->connections);
    }

    for (uint64_t i = 0; i < t->connections; i++) {
        oconn *c = &t->cs[i];
        memset(c, 0, sizeof(*c));
        c->conn.fd = -1;
        c->thread  = t;
        rate_conn_init(&c->rate, t->throughput, now);
        /* Stagger connects 5 ms apart within the thread. */
        aeCreateTimeEvent(t->loop, (long long)i * 5,
                          delayed_initial_connect, c, NULL);
    }

    uint64_t calibrate_delay = CALIBRATE_DELAY_MS + (t->connections * 5);
    uint64_t timeout_delay   = TIMEOUT_INTERVAL_MS + (t->connections * 5);
    aeCreateTimeEvent(t->loop, calibrate_delay, calibrate, t, NULL);
    aeCreateTimeEvent(t->loop, timeout_delay, check_timeouts, t, NULL);

    /* t044: signal that this worker has finished setup and is about to start
     * driving load. orchestrator_run() blocks on this barrier before capturing
     * run_start, so the measured window begins at actual load-start (when every
     * worker is in its loop with connects scheduled), not merely when
     * pthread_create returned. pthread_create returning does NOT mean the thread
     * is running; capturing run_start too early lengthens the measured window
     * and under-reports Requests/sec. Phase-0 wrk.c gets the same effect
     * implicitly: its create loop runs script_create() per thread, so by the
     * time it captures `start` the workers have already spun up. */
    __sync_fetch_and_add(&o->workers_ready, 1);

    aeMain(t->loop);

    /* Drain: close any still-open connections. */
    protocol *p = o->proto;
    for (uint64_t i = 0; i < t->connections; i++) {
        oconn *c = &t->cs[i];
        if (c->conn.fd >= 0) {
            aeDeleteFileEvent(t->loop, c->conn.fd, AE_WRITABLE | AE_READABLE);
            p->close(&c->conn);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Report stage (CLI-side reporter, adapted from wrk.c)                      */
/* ------------------------------------------------------------------------- */

static void print_rule(void) {
    struct winsize ws;
    int w = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            ? ws.ws_col : 100;
    for (int i = 0; i < w; i++) putchar('-');
    putchar('\n');
}

static void print_section(const char *name) {
    printf("\n%s\n", name);
    print_rule();
}

static void print_stats_header(void) {
    printf("%-14s%-12s%-12s%-12s%s\n",
           "Thread Stats", "Avg", "Stdev", "Max", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (len >= 1 && ((msg[len-1] >= 'a' && msg[len-1] <= 'z') ||
                     (msg[len-1] >= 'A' && msg[len-1] <= 'Z'))) pad--;
    if (len >= 2 && ((msg[len-2] >= 'a' && msg[len-2] <= 'z') ||
                     (msg[len-2] >= 'A' && msg[len-2] <= 'Z'))) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");
    free(msg);
}

static void print_stats(const char *name, stats *s, char *(*fmt)(long double)) {
    uint64_t max    = s->max;
    long double mean  = stats_summarize(s);
    long double stdev = stats_stdev(s, mean);

    char *smean  = fmt(mean);
    char *sstdev = fmt(stdev);
    char *smax   = fmt((long double)max);

    printf("%-14s%-12s%-12s%-12s%.2Lf%%\n",
           name, smean, sstdev, smax,
           stats_within_stdev(s, mean, stdev, 1));

    free(smean);
    free(sstdev);
    free(smax);
}

static void print_hdr_latency(struct hdr_histogram *h, const char *desc) {
    long double pcts[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0 };
    printf("Latency Distribution (HdrHistogram - %s)\n", desc);
    for (size_t i = 0; i < sizeof(pcts) / sizeof(pcts[0]); i++) {
        int64_t n = hdr_value_at_percentile(h, (double)pcts[i]);
        printf("%7.3Lf%%", pcts[i]);
        print_units((long double)n, format_time_us, 10);
        printf("\n");
    }
}

/* -------------------------------------------------------------------------
 * Progress thread: calibration bar → run bar (ported from wrk.c)
 * ---------------------------------------------------------------------- */

typedef struct {
    orchestrator *o;
    volatile int  done;
} oprg_arg;

static void *progress_main(void *raw) {
    oprg_arg     *arg       = raw;
    orchestrator *o         = arg->o;
    int           bar_width = 20;
    uint64_t      n         = o->n_threads;
    uint64_t      cal_total = CALIBRATE_DELAY_MS / 1000;

    /* Phase 1 — calibration bar */
    for (uint64_t s = 0; s <= cal_total; s++) {
        if (arg->done) return NULL;
        if (__sync_fetch_and_add(&o->calibrated_threads, 0) >= (int)n)
            break;

        double pct    = (double)s / (double)cal_total;
        int    filled = (int)(pct * bar_width);
        printf("\rCalibrating: [");
        for (int i = 0; i < bar_width; i++) {
            if      (i < filled)               putchar('=');
            else if (i == filled && pct < 1.0) putchar('>');
            else                               putchar(' ');
        }
        printf("] %3d%% (%" PRIu64 "s / %" PRIu64 "s)  ",
               (int)(pct * 100.0), s, cal_total);
        fflush(stdout);
        if (s == cal_total) break;
        sleep(1);
    }

    /* Phase 2 — wait for stragglers, flush cal messages */
    while (!arg->done &&
           __sync_fetch_and_add(&o->calibrated_threads, 0) < (int)n)
        usleep(100000);

    printf("\r%60s\r", "");
    fflush(stdout);
    for (uint64_t i = 0; i < n; i++) {
        if (o->threads[i].cal_msg[0])
            printf("%s\n", o->threads[i].cal_msg);
    }
    fflush(stdout);

    if (arg->done) return NULL;

    /* Phase 3 — run bar */
    uint64_t bar_start = time_us();
    uint64_t stop_at_snap = __atomic_load_n((uint64_t *)&arg->o->stop_at, __ATOMIC_ACQUIRE);
    uint64_t total_us  = stop_at_snap > bar_start ? stop_at_snap - bar_start : 1;

    while (!arg->done) {
        uint64_t now     = time_us();
        uint64_t elapsed = now > bar_start ? now - bar_start : 0;
        if (elapsed > total_us) elapsed = total_us;

        double   pct    = (double)elapsed / (double)total_us;
        int      filled = (int)(pct * bar_width);
        uint64_t el_s   = elapsed  / 1000000;
        uint64_t tot_s  = (total_us + 999999) / 1000000;

        printf("\rProgress: [");
        for (int i = 0; i < bar_width; i++) {
            if      (i < filled)               putchar('=');
            else if (i == filled && pct < 1.0) putchar('>');
            else                               putchar(' ');
        }
        printf("] %3d%% (%" PRIu64 "s / %" PRIu64 "s)  ",
               (int)(pct * 100.0), el_s, tot_s);
        fflush(stdout);

        if (pct >= 1.0) break;
        sleep(1);
    }

    printf("\r%60s\r", "");
    fflush(stdout);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

orchestrator *orchestrator_create(orchestrator_cfg cfg,
                                  protocol *proto,
                                  const script_api *api,
                                  script_engine *engine) {
    if (!proto) return NULL;
    if (cfg.threads == 0) cfg.threads = 1;
    if (cfg.connections == 0) cfg.connections = cfg.threads;
    if (cfg.connections < cfg.threads) cfg.connections = cfg.threads;

    orchestrator *o = calloc(1, sizeof(*o));
    if (!o) return NULL;

    o->cfg    = cfg;
    o->proto  = proto;
    o->engine = engine;
    /* ADR 0002 Decision 1: script_api * is now passed explicitly at creation
     * time. NULL is valid (unit-test stub path); all callers that guard on
     * o->api remain correct. */
    o->api    = api;
    o->record_all_responses = true;
    o->timeout_ms = DEFAULT_TIMEOUT_MS;
    o->stop = 0;
    o->calibrated_threads = 0;

    pthread_mutex_init(&o->rps_mutex, NULL);
    o->rps = stats_alloc(10);
    hdr_init(1, MAX_LATENCY, 3, &o->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &o->u_latency_histogram);

    o->n_threads = cfg.threads;
    o->threads   = calloc(cfg.threads, sizeof(othread));
    if (!o->threads || !o->rps || !o->latency_histogram ||
        !o->u_latency_histogram) {
        orchestrator_destroy(o);
        return NULL;
    }

    uint64_t per_thread_conns = cfg.connections / cfg.threads;
    if (per_thread_conns == 0) per_thread_conns = 1;
    o->total_connections = per_thread_conns * cfg.threads;
    double throughput_per_thread = (double)cfg.rate / cfg.threads;

    uint32_t caps = (api && api->capabilities && engine)
                  ? api->capabilities(engine) : 0;
    bool dyn = (caps & SCRIPT_CAP_DYNAMIC_REQUEST) != 0;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        othread *t = &o->threads[i];
        t->orch        = o;
        t->id          = i;
        t->connections = per_thread_conns;
        t->throughput  = (throughput_per_thread / 1000000.0) / per_thread_conns;

        if (i == 0 || !dyn || !api || !api->clone) {
            t->engine = engine;
        } else {
            t->engine = api->clone(engine);
            if (!t->engine) { t->engine = engine; dyn = false; }
        }

        t->dynamic        = dyn;
        t->wants_response = dyn && (caps & SCRIPT_CAP_RESPONSE_HOOK) != 0;

        t->cs = calloc(per_thread_conns, sizeof(oconn));
        if (!t->cs) {
            orchestrator_destroy(o);
            return NULL;
        }
    }

    return o;
}

int orchestrator_run(orchestrator *o) {
    if (!o) return -1;

    uint64_t duration_us = o->cfg.duration_us;
    o->start_us = time_us();
    o->stop_at  = o->start_us + duration_us; /* initial; shifted after conns ready */

    /* Install the signal-driven drain (SIGINT -> graceful stop). */
    g_active = o;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* Init the template engine (thread 0, connections = per-thread split).
     * Must run before static pre-generation: request() in Lua may depend on
     * state set by wrk.init (called inside api->init). Per-clone init runs
     * in thread_main after clone assignment (deliverable 4a). */
    uint64_t per_thread_conns_run = o->cfg.connections / o->n_threads;
    if (per_thread_conns_run == 0) per_thread_conns_run = 1;
    if (o->api && o->api->init && o->engine)
        o->api->init(o->engine, 0, per_thread_conns_run);

    /* t049-fix: generate the static request buffer once, single-threaded, before
     * any worker is spawned. api->request() calls into the lua_State; calling it
     * from N threads simultaneously is a data race (LuaJIT is not thread-safe)
     * that manifests as an intermittent segfault at calibration startup. Workers
     * read o->static_request_buf as a read-only pointer — no lock needed.
     * Skip in dynamic mode: each cloned engine generates its own requests. */
    bool dynamic_workload = o->n_threads > 0 && o->threads[0].dynamic;
    if (!dynamic_workload && o->api && o->api->request && o->engine) {
        o->static_request_buf = o->api->request(o->engine, &o->static_request_len);
    }

    /* Warn if the per-connection inter-request interval exceeds the test duration.
     * In that case every connection fires at most once (the initial burst) and
     * the measurement window will be empty or nearly empty.
     * Condition: total_connections / rate > duration_s
     *   i.e. each connection fires less than once over the whole test. */
    if (o->cfg.rate > 0 &&
        (double)o->total_connections / (double)o->cfg.rate >
        (double)duration_us / 1e6) {
        double interval_s = (double)o->total_connections / (double)o->cfg.rate;
        fprintf(stderr,
            "Warning: at -R%"PRIu64" with %"PRIu64" connections each connection "
            "fires once every %.1fs; the %"PRIu64"s test is shorter than that "
            "interval so only the initial burst will appear in the measurement "
            "window. Reduce -c or increase -R or -d.\n",
            o->cfg.rate, o->total_connections, interval_s,
            duration_us / 1000000);
    }

    /* Warn if the per-process fd limit is too low to open all requested sockets.
     * Each connection needs 1 fd; the process itself uses a handful (stdin/out/err,
     * kqueue/epoll, pipes, etc.).  Reserve FD_OVERHEAD for those. */
#define FD_OVERHEAD 16
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
            uint64_t available = rl.rlim_cur > FD_OVERHEAD
                                 ? rl.rlim_cur - FD_OVERHEAD : 0;
            if (o->cfg.connections > available) {
                fprintf(stderr,
                    "Warning: %llu connections requested but fd limit is %llu "
                    "(ulimit -n).  Up to %llu connections will fail with EMFILE "
                    "and be permanently idle.  Run: ulimit -n %llu\n",
                    (unsigned long long)o->cfg.connections,
                    (unsigned long long)rl.rlim_cur,
                    (unsigned long long)(o->cfg.connections - available),
                    (unsigned long long)(o->cfg.connections + FD_OVERHEAD));
            }
        }
    }
#undef FD_OVERHEAD

    oprg_arg parg = { o, 0 };
    pthread_t progress_thread;
    pthread_create(&progress_thread, NULL, progress_main, &parg);

    for (uint64_t i = 0; i < o->n_threads; i++) {
        othread *t = &o->threads[i];
        t->loop = aeCreateEventLoop(10 + (int)(o->cfg.connections * 3));
        if (!t->loop) return -2;
        if (pthread_create(&t->thread, NULL, thread_main, t) != 0)
            return -3;
    }

    /* Wait until every worker has entered its event loop (workers_ready barrier,
     * t044), then additionally wait until every initial connection has been
     * terminally resolved — either successfully connected or permanently abandoned.
     * The stagger (5 ms × connections_per_thread) causes a ramp-up window where
     * actual throughput is below -R; by starting the measurement clock here we
     * give the full -d duration of steady-state load and eliminate the ramp-up
     * deficit from the reported Requests/sec.
     *
     * After the barrier we shift o->stop_at forward by the ramp-up time so
     * threads continue running for the full requested duration, then snapshot
     * each thread's cumulative counts so we can subtract the pre-ready portion
     * at aggregation time. */
    while (__sync_fetch_and_add(&o->workers_ready, 0) < (int)o->n_threads)
        sched_yield();
    o->workers_start_us = time_us();  /* ramp-up begins here */

    while (__atomic_load_n((uint64_t *)&o->connections_ready_at, __ATOMIC_ACQUIRE) == 0)
        sched_yield();

    uint64_t run_start = o->connections_ready_at;
    __atomic_store_n((uint64_t *)&o->stop_at, run_start + duration_us, __ATOMIC_RELEASE);

    for (uint64_t i = 0; i < o->n_threads; i++)
        pthread_join(o->threads[i].thread, NULL);

    /* ADR 0003 / t041: close the measured window the instant the workers stop,
     * BEFORE tearing down the progress thread. The progress thread re-checks
     * completion only once per sleep(1) and then erases its line (terminal
     * I/O); joining it first would charge that wakeup lag + erase to runtime_us
     * (~0.3% over 20s), under-reporting Requests/sec. Pre-progress-bar phase-0
     * (and the t040 baseline) read runtime immediately after the worker join. */
    /* Cap at duration_us: the defined window is [connections_ready_at, stop_at].
     * Threads overshoot stop_at by up to one response latency (the check fires
     * at completion, not mid-flight), so the raw wall-clock gap is slightly
     * larger. Using duration_us keeps the denominator exact. */
    uint64_t elapsed = time_us() - run_start;
    if (elapsed > duration_us) elapsed = duration_us;

    parg.done = 1;
    pthread_join(progress_thread, NULL);

    /* Aggregate per-thread stats. Measurement counters (complete, bytes) are
     * gated on the [connections_ready_at, stop_at) window inside record_response.
     * Ramp-up counters collect the excluded pre-ready completions separately. */
    uint64_t complete = 0, bytes = 0;
    uint64_t ramp_complete = 0, ramp_bytes = 0;
    errors agg = {0};
    for (uint64_t i = 0; i < o->n_threads; i++) {
        othread *t = &o->threads[i];
        complete      += t->complete;
        bytes         += t->bytes;
        ramp_complete += t->ramp_complete;
        ramp_bytes    += t->ramp_bytes;
        agg.connect            += t->errors.connect;
        agg.connect_recovered  += t->errors.connect_recovered;
        agg.connect_abandoned  += t->errors.connect_abandoned;
        agg.read               += t->errors.read;
        agg.write              += t->errors.write;
        agg.timeout            += t->errors.timeout;
        agg.status             += t->errors.status;
        if (t->latency_histogram)
            hdr_add(o->latency_histogram, t->latency_histogram);
        if (t->u_latency_histogram)
            hdr_add(o->u_latency_histogram, t->u_latency_histogram);
    }

    o->result.latency        = o->latency_histogram;
    o->result.requests       = complete;
    o->result.errors_connect = agg.connect;
    o->result.errors_status  = agg.status;
    o->result.errors_timeout = agg.timeout;
    o->result.start_us       = run_start;
    o->result.elapsed_us     = elapsed;

    /* ---- Report stage --------------------------------------------------- */
    {
        stats *latency_stats = stats_alloc(10);
        if (latency_stats) {
            latency_stats->min = hdr_min(o->latency_histogram);
            latency_stats->max = hdr_max(o->latency_histogram);
            latency_stats->histogram = o->latency_histogram;

            print_section("Latency");
            print_stats_header();
            print_stats("Latency", latency_stats, format_time_us);
            print_stats("Req/Sec", o->rps, format_metric);

            if (o->cfg.latency) {
                printf("\n");
                print_hdr_latency(o->latency_histogram, "Recorded Latency");
            }
            if (o->cfg.u_latency) {
                printf("\n");
                print_hdr_latency(o->u_latency_histogram,
                                  "Uncorrected Latency (measured without taking "
                                  "delayed starts into account)");
            }

            /* Latency spectrum — only when full spectrum requested (-L / -U
             * without -l).  Both histograms share the same section header. */
            bool has_spectrum = !o->cfg.latency_dist_only &&
                                (o->cfg.latency || o->cfg.u_latency);
            if (has_spectrum) {
                print_section("Latency spectrum");
                if (o->cfg.latency) {
                    printf("Detailed Percentile spectrum:\n");
                    hdr_percentiles_print(o->latency_histogram,
                                         stdout, 5, 1000.0, CLASSIC);
                }
                if (o->cfg.u_latency) {
                    if (o->cfg.latency) printf("\n");
                    printf("Uncorrected Detailed Percentile spectrum:\n");
                    hdr_percentiles_print(o->u_latency_histogram,
                                         stdout, 5, 1000.0, CLASSIC);
                }
            }

            print_section("Summary");

            long double runtime_s = elapsed / 1000000.0;
            long double req_per_s = runtime_s > 0 ? complete / runtime_s : 0;

            /* Two-line summary: measurement window (basis for RPS/Transfer)
             * followed by full-test totals including ramp-up, so both the
             * calculation inputs and the excluded data are visible. */
            uint64_t ramp_us    = run_start > o->workers_start_us
                                  ? run_start - o->workers_start_us : 0;
            uint64_t total_reqs = complete + ramp_complete;
            uint64_t total_us   = elapsed  + ramp_us;

            char *window_time = format_time_us((long double)elapsed);
            char *window_read = format_binary((long double)bytes);
            printf("Measurement: %" PRIu64 " requests in %s, %sB read\n",
                   complete, window_time, window_read);
            free(window_time);
            free(window_read);

            char *total_time = format_time_us((long double)total_us);
            char *total_read = format_binary((long double)(bytes + ramp_bytes));
            char *ramp_time  = format_time_us((long double)ramp_us);
            printf("Total:       %" PRIu64 " requests in %s, %sB read"
                   " (+ %s ramp-up, %" PRIu64 " req excluded)\n",
                   total_reqs, total_time, total_read, ramp_time, ramp_complete);
            free(total_time);
            free(total_read);
            free(ramp_time);

            printf("Requests/sec: %8.2Lf\n", req_per_s);
            char *bps = format_binary(runtime_s > 0 ? bytes / runtime_s : 0);
            printf("Transfer/sec: %9sB\n", bps);
            free(bps);

            if (agg.connect || agg.read || agg.write || agg.timeout) {
                printf("Socket errors: connect %u, read %u, write %u, "
                       "timeout %u\n",
                       agg.connect, agg.read, agg.write, agg.timeout);
                if (agg.connect)
                    printf("  Connect: %u recovered, %u abandoned"
                           " (%u retrying at end)\n",
                           agg.connect_recovered, agg.connect_abandoned,
                           agg.connect - agg.connect_recovered
                                       - agg.connect_abandoned);
            }
            if (agg.status)
                printf("Non-2xx or 3xx responses: %u\n", agg.status);

            stats_free(latency_stats);  /* zcalloc'd — free() crashes on glibc */
        }
    }

    /* Notify the Request Layer the run is done. */
    const script_api *api = engine_api(o);
    if (api && api->done && o->engine)
        api->done(o->engine, &o->result);

    g_active = NULL;
    return 0;
}

orchestrator_stats orchestrator_collect(orchestrator *o) {
    if (!o) {
        orchestrator_stats empty = {0};
        return empty;
    }
    return o->result;
}

void orchestrator_destroy(orchestrator *o) {
    if (!o) return;
    if (o->threads) {
        for (uint64_t i = 0; i < o->n_threads; i++) {
            othread *t = &o->threads[i];
            if (t->loop) aeDeleteEventLoop(t->loop);
            free(t->cs);
            if (t->latency_histogram)   free(t->latency_histogram);
            if (t->u_latency_histogram) free(t->u_latency_histogram);
            /* Free per-thread clone engines; skip the template (o->engine). */
            if (t->engine && t->engine != o->engine && o->api && o->api->destroy)
                o->api->destroy(t->engine);
        }
        free(o->threads);
    }
    if (o->rps) stats_free(o->rps);   /* zcalloc'd — free() crashes on glibc */
    if (o->latency_histogram)   free(o->latency_histogram);
    if (o->u_latency_histogram) free(o->u_latency_histogram);
    free(o->static_request_buf);      /* generated once pre-spawn; free once here */
    pthread_mutex_destroy(&o->rps_mutex);
    free(o);
}
