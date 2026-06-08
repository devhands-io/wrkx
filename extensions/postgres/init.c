/*
 * PostgreSQL extension entry point (ADR 0005, Phase 6, P6-1 + P6-2 + P6-3).
 *
 * Registers the PostgreSQL protocol vtable, URL schemas, and helpers
 * via the public extension API.  Called once at startup by
 * wrkx_register_all_extensions().
 */

#include "wrkx_extension.h"
#include "postgres.h"
#include "pg_lua_helpers.h"
#include "pg_quickjs_helpers.h"

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

    api->register_helpers("postgres@lua",
                          postgres_lua_helpers, postgres_lua_helpers_count);

#ifdef WRKX_HAVE_QUICKJS
    api->register_helpers("postgres@quickjs",
                          postgres_quickjs_helpers,
                          postgres_quickjs_helpers_count);
#endif

    /* Register plain and TLS schemas.
     * Passing "postgres+tls" as schema_tls causes the host to set ssl_ctx
     * when the user supplies postgres+tls://.  detect_protocol() checks the
     * plain schema first; using it as both args would match as plain and
     * leave ssl_ctx NULL.  connect() checks ssl_ctx to decide whether to
     * send the SSLRequest prelude. */
    api->register_schema("postgres",   "postgres+tls",   "5432",
                         postgres_configure_cb);
    api->register_schema("postgresql", "postgresql+ssl",  "5432",
                         postgres_configure_cb);
}
