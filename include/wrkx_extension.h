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

#define WRKX_EXTENSION_API_VERSION  UINT32_C(1)

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
 * Extension registration API
 * ---------------------------------------------------------------------- */

typedef struct wrkx_extension_api {
    uint32_t version;   /* == WRKX_EXTENSION_API_VERSION; check before use  */

    /* Register a protocol vtable with the host. Called once per protocol
     * at startup; the host takes ownership of the pointer lifetime. */
    void (*register_protocol)(const protocol *proto);

    /* Register a scripting helper namespace. The host defers engine-binding
     * until a scripting engine is created; extensions need not know which
     * engine is in use. Called once per namespace. */
    void (*register_helpers)(const char          *ns,
                             const script_helper *helpers,
                             size_t               count);
} wrkx_extension_api;

/* Every extension entry point must match this signature.
 * Symbol name convention: wrkx_extension_init_<name>. */
typedef void (*wrkx_extension_init_fn)(const wrkx_extension_api *api);

#endif /* WRKX_EXTENSION_H */
