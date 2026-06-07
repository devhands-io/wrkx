/*
 * memcached extension entry point (ADR 0005, Phase 4).
 *
 * Registers the stub protocol vtable and the "memcached" Lua helper namespace.
 * URL schema configuration is deferred to t062 (networking).
 */

#include "wrkx_extension.h"
#include "memcached.h"
#include "mc_lua_helpers.h"

void wrkx_extension_init_memcached(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(memcached_protocol());
    api->register_helpers("memcached", mc_lua_helpers, mc_lua_helpers_count);
}

