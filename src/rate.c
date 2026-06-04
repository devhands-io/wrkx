/*
 * Rate controller sub-module (ADR 0001, Phase 1, Orchestrator layer).
 *
 * Adapted from wrk.c (usec_to_next_send, calibrate, the latency-expectation
 * portion of response_complete). The Coordinated-Omission correction is the
 * wrk2 contribution and is preserved verbatim in behaviour.
 *
 * Invariant 1: this file must not include any protocol or scripting header
 * other than proto.h / script_api.h. It includes neither — it depends only on
 * the histogram contract and libc.
 */

#include <math.h>

#include "rate.h"
#include "stats.h"          /* MAX/MIN helpers */
#include "hdr_histogram.h"

void rate_conn_init(rate_conn *rc, double throughput, uint64_t now) {
    rc->throughput                   = throughput;
    rc->catch_up_throughput          = throughput * 2;
    rc->thread_start                 = now;
    rc->complete                     = 0;
    rc->caught_up                    = true;
    rc->catch_up_start_time          = 0;
    rc->complete_at_catch_up_start   = 0;
    rc->complete_at_last_batch_start = 0;
    rc->actual_latency_start         = 0;
    rc->latest_should_send_time      = 0;
    rc->latest_expected_start        = 0;
}

uint64_t rate_usec_to_next_send(rate_conn *rc, uint64_t now) {
    uint64_t next_start_time = rc->thread_start + (rc->complete / rc->throughput);

    bool send_now = true;

    if (next_start_time > now) {
        /* We are on pace. Indicate caught_up and don't send now. */
        rc->caught_up = true;
        send_now = false;
    } else {
        /* We are behind. */
        if (rc->caught_up) {
            /* First fall-behind since we were last caught up. */
            rc->caught_up = false;
            rc->catch_up_start_time = now;
            rc->complete_at_catch_up_start = rc->complete;
        }

        uint64_t complete_since_catch_up_start =
            rc->complete - rc->complete_at_catch_up_start;

        next_start_time = rc->catch_up_start_time +
            (complete_since_catch_up_start / rc->catch_up_throughput);

        if (next_start_time > now) {
            /* Not yet time to send, even at catch-up throughput. */
            send_now = false;
        }
    }

    if (send_now) {
        rc->latest_should_send_time = now;
        rc->latest_expected_start = next_start_time;
    }

    return send_now ? 0 : (next_start_time - now);
}

void rate_begin_batch(rate_conn *rc, uint64_t now) {
    rc->actual_latency_start = now;
    rc->complete_at_last_batch_start = rc->complete;
}

int64_t rate_expected_latency(rate_conn *rc, uint64_t now,
                              int64_t *uncorrected_out) {
    /* Count this completed response. */
    rc->complete++;

    /* Expected start time is computed from the completion count seen at the
     * beginning of the last request batch sent — not the per-response count —
     * so pipelined responses are not "gifted" time (see wrk2 rationale). */
    uint64_t expected_latency_start = rc->thread_start +
        (rc->complete_at_last_batch_start / rc->throughput);

    int64_t expected = (int64_t)now - (int64_t)expected_latency_start;
    if (expected < 0) expected = 0;

    if (uncorrected_out) {
        int64_t actual = (int64_t)now - (int64_t)rc->actual_latency_start;
        if (actual < 0) actual = 0;
        *uncorrected_out = actual;
    }

    rc->latest_should_send_time = 0;
    rc->latest_expected_start = 0;

    return expected;
}

bool rate_calibrate(struct hdr_histogram *latency, uint64_t *mean_out,
                    int *interval_ms) {
    long double mean = hdr_mean(latency);
    /* hdr_mean is total/total_count, so an empty histogram yields NaN. Treat
     * NaN and zero alike: calibration is not yet possible. */
    if (isnan((double)mean) || mean == 0) return false;

    long double p90 = hdr_value_at_percentile(latency, 90.0) / 1000.0L;
    long double interval = MAX(p90 * 2, 10);

    if (mean_out)    *mean_out = (uint64_t)mean;
    if (interval_ms) *interval_ms = (int)interval;
    return true;
}
