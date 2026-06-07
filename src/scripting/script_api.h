#ifndef SCRIPT_API_H
#define SCRIPT_API_H

/*
 * Request Layer contract (ADR 0001, Phase 1; amended by ADR 0002).
 *
 * The Request Layer ("Ammo") owns the scripting engine(s), the hook contract
 * (init/request/response/done), per-protocol helper registration and the
 * per-connection session store. It knows nothing about connections or timing.
 *
 * Invariant 3: scripting/<engine>/engine.c must not #include any protocol
 * header directly. Protocol behaviour is reached only through glue modules.
 * Invariant 4: glue modules (scripting/<engine>/<proto>_helpers.c) are the
 * only place a protocol header and an engine header may coexist.
 *
 * script_helper_fn and script_helper are canonical in include/wrkx_extension.h
 * and re-exported here so internal code continues to work unchanged.
 */

#include <stddef.h>
#include <stdint.h>

#include "wrkx_extension.h"   /* script_helper_fn, script_helper */

/* Defined in orchestrator.h; referenced here by pointer only so this contract
 * does not pull in the Orchestrator header. */
struct orchestrator_stats;

typedef struct script_engine script_engine;
typedef struct session       session;

/* Capability bits an engine reports for the currently loaded script
 * (ADR 0005, Phase 5, t069). Language-neutral: the orchestrator consults these
 * to decide static-vs-dynamic without knowing anything about Lua or JS. */
typedef enum {
    SCRIPT_CAP_DYNAMIC_REQUEST = 1u << 0, /* call request() for every request   */
    SCRIPT_CAP_RESPONSE_HOOK   = 1u << 1, /* deliver response() callbacks        */
} script_cap;

typedef struct script_api {
    const char *name;

    script_engine *(*create)(const char *file);

    /* ADR 0002 Decision 3 — called once per engine after create(), before
     * init().  url is the full target URL (scheme://host:port/path); headers
     * is an array of n_headers raw strings ("X-Foo: bar"); either may be NULL.
     * Returns 0 on success.  May be NULL in the vtable; caller checks first. */
    int (*configure)(script_engine *, const char *url,
                     const char * const *headers, size_t n_headers);

    /* Report capability bits (script_cap) for the loaded script. NULL ⇒ treat
     * as 0 (fully static: pre-generate one request, no response callbacks).
     * Called after configure() and helper registration, before init(). */
    uint32_t (*capabilities)(script_engine *);

    /* Register a namespace of protocol helpers into THIS engine instance.
     * Each engine carries its own implementation (replaces the former global
     * script_register_helpers). NULL ⇒ engine has no helper support. */
    void (*register_helpers)(script_engine *, const char *ns,
                             const script_helper *helpers, size_t count);

    /* Return an independent engine equivalent to `src` AFTER create +
     * register_helpers + configure but BEFORE init(). Called once per worker
     * thread (thread 0 reuses the template). NULL ⇒ engine is not clonable;
     * caller falls back to static mode. */
    script_engine *(*clone)(script_engine *src);

    /* Called once per thread before any requests. */
    void (*init)(script_engine *, uint64_t thread_id, uint64_t connections);

    /* Called before each request. Returns a heap-allocated buffer; caller
     * frees it. */
    char *(*request)(script_engine *, size_t *len_out);

    /* Called after each completed response. status is protocol-defined. */
    void (*response)(script_engine *, int status, size_t bytes,
                     uint64_t latency_us);

    /* Called once after the run completes. */
    void (*done)(script_engine *, struct orchestrator_stats *);

    void (*destroy)(script_engine *);
} script_api;

/* Helper registration is now a per-engine vtable slot (script_api.register_helpers)
 * so multiple engines can coexist in one binary. Each engine exposes its own
 * engine-internal registration entry point for glue modules — for LuaJIT that is
 * lua_register_helpers() in scripting/lua/engine.h. */

/* Session: per-connection key-value store accessible from scripts. */
session    *session_create(void);
void        session_set(session *, const char *key, const char *value);
const char *session_get(session *, const char *key);
void        session_destroy(session *);

#endif /* SCRIPT_API_H */
