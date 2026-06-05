#ifndef PROTO_H
#define PROTO_H

/*
 * Protocol Engine layer contract (ADR 0001, Phase 1).
 *
 * The Protocol Engine ("Machine Gun") owns transport selection (TCP/TLS),
 * the protocol vtable, per-connection protocol state and response-completion
 * detection. It knows nothing about rate control or scripting.
 *
 * Invariant 2: every protocol implementation (proto/<name>.c) must not #include
 * any scripting header (lua.h, quickjs.h, ...). Protocol behaviour is exposed
 * to scripts only through per-engine glue modules, never from here.
 */

#include <stddef.h>

typedef enum {
    PROTO_PENDING,            /* response incomplete; more bytes expected     */
    PROTO_DONE,               /* response complete; application-level success */
    PROTO_DONE_STATUS_ERR,    /* response complete; non-2xx HTTP status       */
    PROTO_DONE_CLOSE,         /* response complete; peer is closing (no
                               * keep-alive) — orchestrator must reconnect
                               * cleanly, NOT count a read error (ADR 0003-B) */
    PROTO_ERROR               /* transport or parse failure                   */
} proto_status;

typedef struct connection connection;

typedef struct protocol {
    const char *name;

    /* Called once per connection. Includes the auth handshake if the protocol
     * requires one. Allocates and assigns conn->proto_state. */
    int (*connect)(connection *);

    /* Encode and send a request. buf/len are owned by the Request Layer. */
    int (*write)(connection *, const char *buf, size_t len);

    /* Called by the event loop on each readable event. Buffers internally.
     * Returns PROTO_DONE when a complete response has arrived, PROTO_PENDING
     * if more bytes are expected, PROTO_ERROR on failure. */
    proto_status (*readable)(connection *);

    /* Frees conn->proto_state and closes the socket. */
    void (*close)(connection *);
} protocol;

/* Shared connection structure. proto_state is owned by the protocol;
 * script_state is owned by the Request Layer. Transport, thread back-pointer
 * and timing fields are internal and added by the implementation. */
struct connection {
    int    fd;
    void  *proto_state;   /* opaque; allocated by connect, freed by close   */
    void  *script_state;  /* opaque; owned by the Request Layer             */
    size_t bytes;         /* response-byte channel (t042): the protocol sets
                           * this to the wire size of the response it just
                           * completed, on every readable() call that returns a
                           * PROTO_DONE* status. The orchestrator reads and
                           * accumulates it when recording that response, then
                           * reports it as Transfer/sec. Undefined for
                           * PROTO_PENDING / PROTO_ERROR returns. */
};

#endif /* PROTO_H */
