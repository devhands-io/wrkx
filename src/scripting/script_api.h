#ifndef SCRIPT_API_H
#define SCRIPT_API_H

/*
 * Request Layer contract (ADR 0001, Phase 1).
 *
 * The Request Layer ("Ammo") owns the scripting engine(s), the hook contract
 * (init/request/response/done), per-protocol helper registration and the
 * per-connection session store. It knows nothing about connections or timing.
 *
 * Invariant 3: scripting/<engine>/engine.c must not #include any protocol
 * header directly. Protocol behaviour is reached only through glue modules.
 * Invariant 4: glue modules (scripting/<engine>/<proto>_helpers.c) are the
 * only place a protocol header and an engine header may coexist.
 */

#include <stddef.h>
#include <stdint.h>

/* Defined in orchestrator.h; referenced here by pointer only so this contract
 * does not pull in the Orchestrator header. */
struct orchestrator_stats;

typedef struct script_engine script_engine;
typedef struct session       session;

typedef struct script_api {
    const char *name;

    script_engine *(*create)(const char *file);

    /* Called once per thread before any requests. */
    void (*init)(script_engine *, uint64_t thread_id, uint64_t connections);

    /* Called before each request. Returns a heap-allocated buffer; the engine
     * frees it. */
    char *(*request)(script_engine *, size_t *len_out);

    /* Called after each completed response. status is protocol-defined. */
    void (*response)(script_engine *, int status, size_t bytes,
                     uint64_t latency_us);

    /* Called once after the run completes. */
    void (*done)(script_engine *, struct orchestrator_stats *);

    void (*destroy)(script_engine *);
} script_api;

/*
 * Engine-agnostic helper descriptor. engine_ctx is supplied by the scripting
 * engine at each call site; its concrete type (lua_State *, JSContext *, ...)
 * is known only to the glue module that implements fn — never to the protocol
 * layer. Argument marshalling and return-value handling are the engine's job.
 */
typedef int (*script_helper_fn)(void *engine_ctx);

typedef struct script_helper {
    const char      *name;
    script_helper_fn fn;   /* implemented in scripting/<engine>/<proto>_helpers.c */
} script_helper;

/*
 * Registers a namespace of helpers into the scripting engine. Called by each
 * scripting/<engine>/<proto>_helpers.c during engine init. NOT called by
 * protocol implementations (proto layer) — they have no scripting knowledge.
 */
void script_register_helpers(script_engine       *,
                             const char          *ns,
                             const script_helper *helpers,
                             size_t               count);

/* Session: per-connection key-value store accessible from scripts. */
session    *session_create(void);
void        session_set(session *, const char *key, const char *value);
const char *session_get(session *, const char *key);
void        session_destroy(session *);

#endif /* SCRIPT_API_H */
