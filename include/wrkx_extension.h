#ifndef WRKX_EXTENSION_H
#define WRKX_EXTENSION_H

/*
 * wrkx public extension API (ADR 0005, Phase 3, P3-1).
 *
 * This is the ONLY wrkx header extensions are permitted to include.
 * It provides everything an extension needs to implement a protocol and
 * register scripting helpers, with no access to private core internals.
 *
 * Runtime version contract: the host passes a wrkx_extension_api * to each
 * extension entry point. Extensions MUST check api->version against
 * WRKX_EXTENSION_API_VERSION before using any other field and silently
 * return without registering anything if the versions differ.
 *
 * See docs/extension-invariants.md for what extensions may not include.
 */

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------- */

#define WRKX_EXTENSION_API_VERSION  UINT32_C(2)

/* -------------------------------------------------------------------------
 * Protocol vtable
 *
 * Canonical definitions — src/proto/proto.h re-exports these.
 * The comment in src/orchestrator.c ("struct connection may not gain fields")
 * is an invariant of the container relation (oconn wraps connection as its
 * first member). Extensions must honour it too.
 * ---------------------------------------------------------------------- */

typedef enum {
    PROTO_PENDING,            /* response incomplete; more bytes expected     */
    PROTO_DONE,               /* response complete; application-level success */
    PROTO_DONE_STATUS_ERR,    /* response complete; protocol-level error      */
    PROTO_DONE_CLOSE,         /* response complete; peer is closing           */
    PROTO_ERROR               /* transport or parse failure                   */
} proto_status;

typedef struct connection connection;

typedef struct protocol {
    const char *name;
    int          (*connect) (connection *);
    int          (*write)   (connection *, const char *buf, size_t len);
    proto_status (*readable)(connection *);
    void         (*close)   (connection *);
} protocol;

struct connection {
    int    fd;
    void  *proto_state;   /* opaque; allocated by connect, freed by close   */
    void  *script_state;  /* opaque; owned by the Request Layer             */
    size_t bytes;         /* wire size of last completed response batch     */
};

/* -------------------------------------------------------------------------
 * Scripting helper types
 *
 * Canonical definitions — src/scripting/script_api.h re-exports these.
 * ---------------------------------------------------------------------- */

typedef int (*script_helper_fn)(void *engine_ctx);

typedef struct script_helper {
    const char       *name;
    script_helper_fn  fn;
} script_helper;

/* -------------------------------------------------------------------------
 * Connection info (passed to schema configure callbacks)
 *
 * The host populates this after URL parsing and address resolution and
 * passes it to the wrkx_configure_fn registered via register_schema().
 * Extensions cast addrinfo / ssl_ctx back to their native types internally
 * (include <netdb.h> and <openssl/ssl.h> in the extension source file).
 * ---------------------------------------------------------------------- */

typedef struct wrkx_connect_info {
    void       *addrinfo;   /* struct addrinfo *; cast after including <netdb.h>  */
    void       *ssl_ctx;    /* SSL_CTX *; cast after including <openssl/ssl.h>;
                             * NULL for plain TCP                                  */
    const char *host;       /* resolved hostname (for SNI etc.)                   */
    const char *password;   /* URL userinfo field; NULL if absent                 */
    const char *path;       /* URL path component (e.g. "/1" for database index)  */
    const char *url;        /* full original URL                                  */
} wrkx_connect_info;

typedef void (*wrkx_configure_fn)(const wrkx_connect_info *info);

/* -------------------------------------------------------------------------
 * Extension registration API
 * ---------------------------------------------------------------------- */

typedef struct wrkx_extension_api {
    uint32_t version;   /* == WRKX_EXTENSION_API_VERSION; check before use  */

    /* Register a protocol vtable with the host. Called once per protocol
     * at startup; the host takes ownership of the pointer lifetime. */
    void (*register_protocol)(const protocol *proto);

    /* Register a scripting helper namespace. The host binds helpers to each
     * scripting engine as it is created. Called once per namespace. */
    void (*register_helpers)(const char          *ns,
                             const script_helper *helpers,
                             size_t               count);

    /* Register URL schemas handled by this extension.
     * schema     — plain variant  (e.g. "redis")
     * schema_tls — TLS variant    (e.g. "rediss"); may be NULL
     * default_port — default service port string (e.g. "6379")
     * configure  — called once after URL resolution with the resolved
     *              address, TLS context, and parsed URL fields; may be NULL
     *              if the extension needs no per-run configuration. */
    void (*register_schema)(const char        *schema,
                            const char        *schema_tls,
                            const char        *default_port,
                            wrkx_configure_fn  configure);
} wrkx_extension_api;

/* Every extension entry point must match this signature.
 * Symbol name convention: wrkx_extension_init_<name>. */
typedef void (*wrkx_extension_init_fn)(const wrkx_extension_api *api);

#endif /* WRKX_EXTENSION_H */
