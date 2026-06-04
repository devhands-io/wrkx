#ifndef RATE_H
#define RATE_H

#include <stdbool.h>
#include <stdint.h>

struct hdr_histogram;

/*
 * Rate controller sub-module (ADR 0001, Phase 1, Orchestrator layer).
 *
 * Owns the open-model send pacing and the Coordinated-Omission correction
 * that wrk2 introduced. Extracted from wrk.c: usec_to_next_send, delay_request,
 * calibrate, check_timeouts, sample_rate.
 *
 * This module knows nothing about protocols, scripting, sockets or the event
 * loop. It operates purely on the per-connection timing state below; the
 * Orchestrator glues it to ae and to the protocol vtable.
 */

/* Per-connection rate / Coordinated-Omission state. The Orchestrator embeds
 * one of these in its own per-connection structure (never in the frozen
 * `struct connection`). */
typedef struct rate_conn {
    double   throughput;             /* requests/usec for this connection      */
    double   catch_up_throughput;    /* faster pace used while behind          */
    uint64_t thread_start;           /* when this connection's clock began     */
    uint64_t complete;               /* requests completed on this connection  */

    bool     caught_up;
    uint64_t catch_up_start_time;
    uint64_t complete_at_catch_up_start;

    /* Latency bookkeeping for CO correction. */
    uint64_t complete_at_last_batch_start;
    uint64_t actual_latency_start;

    /* Debug/trace fields (kept for parity with wrk2). */
    uint64_t latest_should_send_time;
    uint64_t latest_expected_start;
} rate_conn;

/* Initialise a connection's rate state for a given per-connection throughput
 * (requests per microsecond). */
void rate_conn_init(rate_conn *rc, double throughput, uint64_t now);

/*
 * Returns the number of microseconds to wait before the next request may be
 * sent on this connection, or 0 if a request should be sent now. Mirrors
 * wrk2's usec_to_next_send open-model + catch-up logic.
 */
uint64_t rate_usec_to_next_send(rate_conn *rc, uint64_t now);

/*
 * Computes the Coordinated-Omission-corrected expected latency for a response
 * completing at `now`, advances the per-connection completion counter and
 * resets the should-send trackers. `*uncorrected_out` receives the raw
 * (measured) latency. The two histograms are the caller's to record into.
 */
int64_t rate_expected_latency(rate_conn *rc, uint64_t now,
                              int64_t *uncorrected_out);

/* Marks the start of a request batch (called on first byte of a send). */
void rate_begin_batch(rate_conn *rc, uint64_t now);

/* Per-thread rate-sampling interval calibration. Returns true once a non-zero
 * mean latency is available (calibration complete); fills *interval_ms with the
 * sampling interval and *mean_out with the mean latency in microseconds. */
bool rate_calibrate(struct hdr_histogram *latency, uint64_t *mean_out,
                    int *interval_ms);

#endif /* RATE_H */
