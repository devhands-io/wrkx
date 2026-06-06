/*
 * Redis extension entry point (ADR 0005, Phase 3, P3-3).
 *
 * Registers the Redis protocol vtable, URL schemas, and Lua helpers via the
 * public extension API. Called once at startup by wrkx_register_all_extensions().
 */

#include "wrkx_extension.h"
#include "redis.h"
#include "redis_lua_helpers.h"

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
    api->register_helpers("redis", redis_lua_helpers, redis_lua_helpers_count);
    api->register_schema("redis", "rediss", "6379", redis_configure_cb);
}
