/*
 * MySQL extension entry point (ADR 0005, Phase 6, P6-4).
 *
 * Registers the MySQL protocol vtable, URL schema, and Lua helpers
 * via the public extension API.  Called once at startup.
 */

#include "wrkx_extension.h"
#include "mysql.h"
#include "mysql_lua_helpers.h"

#include <stdlib.h>
#include <string.h>
#include <netdb.h>

/* -------------------------------------------------------------------------
 * Configure callback — called by the host after URL resolution.
 * ---------------------------------------------------------------------- */

static void mysql_configure_cb(const wrkx_connect_info *info) {
    struct addrinfo *addr = (struct addrinfo *)info->addrinfo;

    const char *user     = NULL;
    const char *password = NULL;
    const char *dbname   = NULL;

    /* Userinfo field from URL parser: "user:password" or "user" */
    const char *userinfo = info->password;   /* host field carries userinfo */
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

    /* Path: strip leading '/' */
    if (info->path && info->path[0] == '/' && info->path[1] != '\0')
        dbname = strdup(info->path + 1);
    else
        dbname = strdup(user);

    mysql_configure(addr, info->host, user, password, dbname);
}

/* -------------------------------------------------------------------------
 * Extension entry point
 * ---------------------------------------------------------------------- */

void wrkx_extension_init_mysql(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(mysql_protocol());

    api->register_helpers("mysql@lua",
                          mysql_lua_helpers, mysql_lua_helpers_count);

    api->register_schema("mysql", NULL, "3306", mysql_configure_cb);
}
