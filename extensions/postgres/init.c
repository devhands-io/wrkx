/*
 * PostgreSQL extension entry point (ADR 0005, Phase 6, P6-1 + P6-2).
 *
 * Registers the PostgreSQL protocol vtable, URL schemas, and Lua helpers
 * via the public extension API.  Called once at startup by
 * wrkx_register_all_extensions().
 */

#include "wrkx_extension.h"
#include "postgres.h"
#include "pg_lua_helpers.h"

#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <openssl/ssl.h>

/* -------------------------------------------------------------------------
 * Configure callback — called by the host after URL resolution.
 * ---------------------------------------------------------------------- */

static void postgres_configure_cb(const wrkx_connect_info *info) {
    struct addrinfo *addr    = (struct addrinfo *)info->addrinfo;
    SSL_CTX         *ssl_ctx = (SSL_CTX *)        info->ssl_ctx;

    const char *user     = NULL;
    const char *password = NULL;
    const char *dbname   = NULL;

    /* info->password is the raw UF_USERINFO field: "user" or "user:password".
     * strndup is POSIX-only; use malloc+memcpy to stay within C99. */
    const char *userinfo = info->password;
    if (userinfo) {
        const char *colon = strchr(userinfo, ':');
        if (colon) {
            size_t ulen = (size_t)(colon - userinfo);
            char  *ubuf = malloc(ulen + 1);
            if (!ubuf) return;
            memcpy(ubuf, userinfo, ulen);
            ubuf[ulen] = '\0';
            user     = ubuf;
            password = strdup(colon + 1);
        } else {
            user     = strdup(userinfo);
            password = NULL;
        }
    } else {
        user     = strdup("wrkx");
        password = NULL;
    }

    /* path is "/dbname" — strip the leading slash. Default to user. */
    if (info->path && info->path[0] == '/' && info->path[1] != '\0')
        dbname = strdup(info->path + 1);
    else
        dbname = strdup(user);

    postgres_configure(addr, ssl_ctx, info->host, user, password, dbname);
}

/* -------------------------------------------------------------------------
 * Extension entry point
 * ---------------------------------------------------------------------- */

void wrkx_extension_init_postgres(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(postgres_protocol());

    /* @lua tag: helper bodies cast engine_ctx to lua_State * (Lua-shaped);
     * host binds them only to the LuaJIT engine. */
    api->register_helpers("postgres@lua",
                          postgres_lua_helpers, postgres_lua_helpers_count);

    /* TLS schemas (postgres+tls://, postgresql+ssl://) deferred to P6-3
     * — PostgreSQL requires an SSLRequest prelude before the TLS handshake,
     * which the current transport path does not implement. */
    api->register_schema("postgres",   NULL, "5432", postgres_configure_cb);
    api->register_schema("postgresql", NULL, "5432", postgres_configure_cb);
}
