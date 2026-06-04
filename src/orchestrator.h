#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

/*
 * Orchestrator layer contract (ADR 0001, Phase 1; amended by ADR 0002).
 *
 * The Orchestrator ("Tank") owns the thread pool, connection pool, rate
 * controller (with Coordinated-Omission correction), stats aggregation and
 * the run lifecycle (init -> connect -> run -> drain -> report). It knows
 * nothing about protocols or request content: it asks the Protocol Engine
 * "is the response complete?" and the Request Layer "what bytes next?".
 *
 * Invariant 1: orchestrator.c must not #include any protocol or scripting
 * header other than proto.h and script_api.h.
 */

#include <stdint.h>

/* Pointer-only references — kept decoupled via forward declarations so this
 * header does not pull in the protocol, scripting, or histogram contracts.
 * The concrete definitions live in proto/proto.h, scripting/script_api.h and
 * hdr_histogram.h respectively. */
struct hdr_histogram;
struct protocol;
struct script_api;     /* ADR 0002: vtable passed to orchestrator_create */
struct script_engine;

typedef struct orchestrator_cfg {
    uint64_t connections;
    uint64_t threads;
    uint64_t duration_us;
    uint64_t rate;
} orchestrator_cfg;

typedef struct orchestrator_stats {
    struct hdr_histogram *latency;
    uint64_t requests;
    uint64_t errors_connect;
    uint64_t errors_status;
    uint64_t errors_timeout;
    uint64_t start_us;
    uint64_t elapsed_us;
} orchestrator_stats;

/* Opaque handle: all orchestrator state (formerly the cfg/statistics/stop/
 * g_calibrated_threads/g_progress_done globals) is folded in here. */
typedef struct orchestrator orchestrator;

/* ADR 0002 Decision 1: script_api * (vtable) is passed alongside the engine
 * so the orchestrator can call all scripting hooks without an extra accessor.
 * The script_api * is the vtable; script_engine * is the per-run instance
 * created by api->create().  Unit-test stubs may pass NULL for both. */
orchestrator      *orchestrator_create(orchestrator_cfg,
                                       struct protocol *,
                                       const struct script_api *,
                                       struct script_engine *);
int                orchestrator_run(orchestrator *);
orchestrator_stats orchestrator_collect(orchestrator *);
void               orchestrator_destroy(orchestrator *);

#endif /* ORCHESTRATOR_H */
