/*
 * memcached extension entry point (ADR 0005, Phase 4, P4-1).
 *
 * Registers a stub protocol vtable through the public extension API. Command
 * codecs, Lua helpers, and URL schema configuration are intentionally deferred.
 */

#include "wrkx_extension.h"
#include "memcached.h"

void wrkx_extension_init_memcached(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(memcached_protocol());
}

