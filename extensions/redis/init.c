/*
 * Redis extension entry point (ADR 0005, Phase 3, P3-3; updated t075).
 *
 * Registers the Redis protocol vtable, URL schemas, Lua helpers, and
 * (when built with QuickJS) QuickJS helpers via the public extension API.
 * Called once at startup by wrkx_register_all_extensions().
 */

#include "wrkx_extension.h"
#include "redis.h"
#include "redis_lua_helpers.h"
#include "redis_quickjs_helpers.h"   /* no-op when WRKX_HAVE_QUICKJS not set */

#include <stdlib.h>
#include <netdb.h>
#include <openssl/ssl.h>

/* -------------------------------------------------------------------------
 * Configure callback — called by the host after URL resolution.
 * ---------------------------------------------------------------------- */

static void redis_configure_cb(const wrkx_connect_info *info) {
    struct addrinfo *addr    = (struct addrinfo *) info->addrinfo;
    SSL_CTX         *ssl_ctx = (SSL_CTX *)         info->ssl_ctx;

    int db = 0;
    if (info->path && info->path[0] == '/' && info->path[1] != '\0')
        db = atoi(info->path + 1);

    redis_configure(addr, ssl_ctx, info->host, info->password, db);
}

/* -------------------------------------------------------------------------
 * Extension entry point
 * ---------------------------------------------------------------------- */

void wrkx_extension_init_redis(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(redis_protocol());

    /* @lua tag: these helper bodies cast engine_ctx to lua_State * (Lua-shaped),
     * so the host binds them only to the LuaJIT engine (ADR 0005, Phase 5, t069). */
    api->register_helpers("redis@lua", redis_lua_helpers, redis_lua_helpers_count);

#ifdef WRKX_HAVE_QUICKJS
    /* @quickjs tag: these helper bodies unpack qjs_helper_ctx * (t075). */
    api->register_helpers("redis@quickjs",
                          redis_quickjs_helpers, redis_quickjs_helpers_count);
#endif

    api->register_schema("redis", "rediss", "6379", redis_configure_cb);
}
