/*
 * memcached extension entry point (ADR 0005, Phase 4, t062).
 *
 * Registers the protocol vtable, the "memcached" Lua helper namespace,
 * and the "memcached" URL schema → port 11211.
 */

#include "wrkx_extension.h"
#include "memcached.h"
#include "mc_lua_helpers.h"

#include <netdb.h>
#include <openssl/ssl.h>

static void mc_configure_cb(const wrkx_connect_info *info) {
    struct addrinfo *addr    = (struct addrinfo *) info->addrinfo;
    SSL_CTX         *ssl_ctx = (SSL_CTX *)         info->ssl_ctx;
    memcached_configure(addr, ssl_ctx, info->host, info->password);
}

void wrkx_extension_init_memcached(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(memcached_protocol());
    api->register_helpers("memcached", mc_lua_helpers, mc_lua_helpers_count);
    api->register_schema("memcached", NULL, "11211", mc_configure_cb);
}

