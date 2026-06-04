/*
 * Phase 1 contract stub (ADR 0001, step P1-1).
 *
 * This file exists only to prove the three layer-contract headers compile and
 * co-include cleanly. It declares but does not call anything; no layer
 * implementation exists yet. Task t029 (P1-5) replaces this with the real
 * wiring that selects a protocol + scripting engine and drives the
 * orchestrator lifecycle.
 *
 * Built only by `make contracts-check` — it is intentionally absent from the
 * SRC list, so it never links against wrk.c's main().
 */

#include "orchestrator.h"
#include "proto/proto.h"
#include "scripting/script_api.h"

int main(void) {
    /* Touch one type from each contract so the headers are exercised, not
     * merely preprocessed. No layer functions are called. */
    orchestrator_cfg   cfg   = {0};
    orchestrator_stats stats = {0};
    struct connection  conn  = {0};
    proto_status       st    = PROTO_PENDING;
    protocol          *proto = 0;
    script_engine     *eng   = 0;
    script_helper      helper = {0};
    session           *sess  = 0;

    (void)cfg;
    (void)stats;
    (void)conn;
    (void)st;
    (void)proto;
    (void)eng;
    (void)helper;
    (void)sess;
    return 0;
}
