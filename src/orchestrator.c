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
#define CALIBRATE_DELAY_MS  10000
#define TIMEOUT_INTERVAL_MS 2000
#define DEFAULT_TIMEOUT_MS  2000

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
} oconn;

typedef struct othread {
    pthread_t             thread;
    aeEventLoop          *loop;
    orchestrator         *orch;     /* back-pointer to the handle             */
    uint64_t              id;
    uint64_t              connections;
    double                throughput;    /* per-connection req/usec           */
    uint64_t              stop_at;
    int                   interval;       /* rate sampling interval (ms)       */
    uint64_t              sample_start;
    uint64_t              requests;        /* requests since last sample        */
    uint64_t              complete;        /* total completed on this thread    */
    uint64_t              bytes;
    errors                errors;
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *u_latency_histogram;
    script_engine        *engine;          /* this thread's engine instance     */
    oconn                *cs;
    bool                  dynamic;          /* re-ask script per request?        */
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
    volatile sig_atomic_t   stop;
    uint64_t                start_us;
    bool                    record_all_responses;
    uint64_t                timeout_ms;
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
    if (api && api->response && t->engine)
        api->response(t->engine, status, bytes, latency_us);
}

/* ------------------------------------------------------------------------- */
/* ae glue — thin delegation to the protocol vtable                          */
/* ------------------------------------------------------------------------- */

static int  oc_connect(othread *t, oconn *c);
static int  oc_reconnect(othread *t, oconn *c);
static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask);
static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask);

static int delayed_initial_connect(aeEventLoop *loop, long long id, void *data) {
    (void)loop; (void)id;
    oconn *c = data;
    c->rate.thread_start = time_us();
    oc_connect(c->thread, c);
    return AE_NOMORE;
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

static void complete_response(othread *t, oconn *c) {
    protocol *p = t->orch->proto;
    orchestrator *o = t->orch;
    uint64_t now = time_us();

    t->complete++;
    t->requests++;

    int64_t uncorrected = 0;
    int64_t expected = rate_expected_latency(&c->rate, now, &uncorrected);

    c->in_flight = false;

    /* The orchestrator core sends one request per batch (pipelining is a
     * protocol concern handled inside proto->readable), so every PROTO_DONE
     * corresponds to one recordable latency sample. record_all_responses is
     * retained for parity with the -B batch-latency option once pipelining is
     * reintroduced at the protocol layer. */
    (void)o->record_all_responses;
    hdr_record_value(t->latency_histogram, expected);
    hdr_record_value(t->u_latency_histogram, uncorrected);

    /* Tell the Request Layer a response completed (status is protocol-defined;
     * the orchestrator does not interpret it). */
    tell_response(t, 0, 0, (uint64_t)expected);

    if (now >= t->stop_at || o->stop) {
        aeStop(t->loop);
        return;
    }

    /* Re-arm the writeable event to send the next request. */
    aeCreateFileEvent(t->loop, c->conn.fd, AE_WRITABLE, socket_writeable, c);
    (void)p;
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

    hdr_reset(t->latency_histogram);
    hdr_reset(t->u_latency_histogram);

    t->interval     = interval;
    t->sample_start = time_us();
    t->requests     = 0;

    snprintf(t->cal_msg, sizeof(t->cal_msg),
             "  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms",
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

    if (o->stop || now >= t->stop_at) {
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

    /* If the script is static, fetch the request bytes once up front. */
    if (!t->dynamic) {
        const script_api *api = engine_api(o);
        if (api && api->request && t->engine) {
            size_t len = 0;
            char *buf = api->request(t->engine, &len);
            t->static_request = buf;
            t->static_length  = len;
        }
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

static void print_stats_header(void) {
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
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
    uint64_t max = s->max;
    long double mean  = stats_summarize(s);
    long double stdev = stats_stdev(s, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(max,   fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(s, mean, stdev, 1));
}

static void print_hdr_latency(struct hdr_histogram *h, const char *desc,
                               bool print_spectrum) {
    long double pcts[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0 };
    printf("  Latency Distribution (HdrHistogram - %s)\n", desc);
    for (size_t i = 0; i < sizeof(pcts) / sizeof(pcts[0]); i++) {
        int64_t n = hdr_value_at_percentile(h, (double)pcts[i]);
        printf("%7.3Lf%%", pcts[i]);
        print_units((long double)n, format_time_us, 10);
        printf("\n");
    }
    if (print_spectrum) {
        printf("\n%s\n", "  Detailed Percentile spectrum:");
        hdr_percentiles_print(h, stdout, 5, 1000.0, CLASSIC);
    }
}

/* -------------------------------------------------------------------------
 * Progress thread: calibration bar → run bar (ported from wrk.c)
 * ---------------------------------------------------------------------- */

typedef struct {
    orchestrator *o;
    uint64_t      stop_at;
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
        printf("\r  Calibrating: [");
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
    uint64_t total_us  = arg->stop_at > bar_start ? arg->stop_at - bar_start : 1;

    while (!arg->done) {
        uint64_t now     = time_us();
        uint64_t elapsed = now > bar_start ? now - bar_start : 0;
        if (elapsed > total_us) elapsed = total_us;

        double   pct    = (double)elapsed / (double)total_us;
        int      filled = (int)(pct * bar_width);
        uint64_t el_s   = elapsed  / 1000000;
        uint64_t tot_s  = total_us / 1000000;

        printf("\r  Progress: [");
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
    double throughput_per_thread = (double)cfg.rate / cfg.threads;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        othread *t = &o->threads[i];
        t->orch        = o;
        t->id          = i;
        t->connections = per_thread_conns;
        /* per-connection throughput in requests/usec */
        t->throughput  = (throughput_per_thread / 1000000.0) / per_thread_conns;
        t->engine      = engine; /* template; real wiring clones per thread   */
        t->dynamic     = false;
        t->cs          = calloc(per_thread_conns, sizeof(oconn));
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
    uint64_t stop_at = o->start_us + duration_us;

    /* Install the signal-driven drain (SIGINT -> graceful stop). */
    g_active = o;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    oprg_arg parg = { o, stop_at, 0 };
    pthread_t progress_thread;
    pthread_create(&progress_thread, NULL, progress_main, &parg);

    for (uint64_t i = 0; i < o->n_threads; i++) {
        othread *t = &o->threads[i];
        t->stop_at = stop_at;
        t->loop = aeCreateEventLoop(10 + (int)(o->cfg.connections * 3));
        if (!t->loop) return -2;
        if (pthread_create(&t->thread, NULL, thread_main, t) != 0)
            return -3;
    }

    /* ADR 0003 Decision A: the reported elapsed window must EXCLUDE the
     * thread/event-loop creation above. `o->start_us` stays the stop_at anchor
     * (the test still stops at a fixed wall-clock duration); `run_start` is the
     * runtime clock captured *after* the create loop — matching phase-0
     * wrk.c's `start = time_us()`. Reporting elapsed from o->start_us instead
     * would charge setup time to the test window and under-report Requests/sec. */
    uint64_t run_start = time_us();

    for (uint64_t i = 0; i < o->n_threads; i++)
        pthread_join(o->threads[i].thread, NULL);

    parg.done = 1;
    pthread_join(progress_thread, NULL);

    uint64_t elapsed = time_us() - run_start;

    /* Aggregate per-thread stats into the handle. */
    uint64_t complete = 0, bytes = 0;
    errors agg = {0};
    for (uint64_t i = 0; i < o->n_threads; i++) {
        othread *t = &o->threads[i];
        complete += t->complete;
        bytes    += t->bytes;
        agg.connect += t->errors.connect;
        agg.read    += t->errors.read;
        agg.write   += t->errors.write;
        agg.timeout += t->errors.timeout;
        agg.status  += t->errors.status;
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

            print_stats_header();
            print_stats("Latency", latency_stats, format_time_us);
            print_stats("Req/Sec", o->rps, format_metric);

            if (o->cfg.latency) {
                print_hdr_latency(o->latency_histogram,
                                  "Recorded Latency",
                                  !o->cfg.latency_dist_only);
                printf("----------------------------------------------------------\n");
            }
            if (o->cfg.u_latency) {
                printf("\n");
                print_hdr_latency(o->u_latency_histogram,
                                  "Uncorrected Latency (measured without taking "
                                  "delayed starts into account)",
                                  !o->cfg.latency_dist_only);
                printf("----------------------------------------------------------\n");
            }

            long double runtime_s = elapsed / 1000000.0;
            long double req_per_s = runtime_s > 0 ? complete / runtime_s : 0;
            char *runtime_msg = format_time_us((long double)elapsed);
            printf("  %" PRIu64 " requests in %s, %sB read\n",
                   complete, runtime_msg, format_binary((long double)bytes));
            free(runtime_msg);

            if (agg.connect || agg.read || agg.write || agg.timeout) {
                printf("  Socket errors: connect %u, read %u, write %u, "
                       "timeout %u\n",
                       agg.connect, agg.read, agg.write, agg.timeout);
            }
            if (agg.status)
                printf("  Non-2xx or 3xx responses: %u\n", agg.status);

            printf("Requests/sec: %9.2Lf\n", req_per_s);
            char *bps = format_binary(runtime_s > 0 ? bytes / runtime_s : 0);
            printf("Transfer/sec: %10sB\n", bps);
            free(bps);

            free(latency_stats);
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
        }
        free(o->threads);
    }
    if (o->rps) free(o->rps);
    if (o->latency_histogram)   free(o->latency_histogram);
    if (o->u_latency_histogram) free(o->u_latency_histogram);
    pthread_mutex_destroy(&o->rps_mutex);
    free(o);
}
