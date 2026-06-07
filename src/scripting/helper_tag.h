#ifndef SCRIPTING_HELPER_TAG_H
#define SCRIPTING_HELPER_TAG_H

/*
 * Engine-tagged helper-namespace selection (ADR 0005, Phase 5, t069).
 *
 * Extensions may register a helper namespace with an "@engine" suffix
 * ("redis@lua") so a Lua-shaped helper table is bound only to the matching
 * engine — its function bodies assume a particular engine_ctx and would crash
 * under a different VM. An untagged namespace ("redis") is engine-agnostic and
 * binds to every engine.
 *
 * helper_ns_select() is the single predicate the host applies per registered
 * namespace. Header-only and dependency-free so both main.c and the unit tests
 * exercise the exact same logic.
 */

#include <stdbool.h>
#include <string.h>

/*
 * Decide whether namespace `ns` should be bound for the engine named
 * `engine_name`. On true, `bare` receives `ns` with any "@engine" suffix
 * stripped (the namespace the script ultimately sees). On false, `bare` is
 * left untouched and the caller skips this namespace.
 *
 *   "redis"          , "lua"     -> true,  bare = "redis"   (untagged: all engines)
 *   "redis@lua"      , "lua"     -> true,  bare = "redis"
 *   "redis@quickjs"  , "lua"     -> false
 */
static inline bool helper_ns_select(const char *ns, const char *engine_name,
                                    char *bare, size_t barecap) {
    if (ns == NULL || bare == NULL || barecap == 0) return false;

    const char *at = strchr(ns, '@');
    if (at == NULL) {                       /* untagged: applies to every engine */
        size_t n = strlen(ns);
        if (n >= barecap) n = barecap - 1;
        memcpy(bare, ns, n);
        bare[n] = '\0';
        return true;
    }

    if (engine_name == NULL || strcmp(at + 1, engine_name) != 0)
        return false;                       /* tagged for a different engine */

    size_t n = (size_t)(at - ns);
    if (n >= barecap) n = barecap - 1;
    memcpy(bare, ns, n);
    bare[n] = '\0';
    return true;
}

#endif /* SCRIPTING_HELPER_TAG_H */
