#ifndef PROTO_H
#define PROTO_H

/*
 * Protocol Engine layer contract (ADR 0001, Phase 1).
 *
 * The Protocol Engine ("Machine Gun") owns transport selection (TCP/TLS),
 * the protocol vtable, per-connection protocol state and response-completion
 * detection. It knows nothing about rate control or scripting.
 *
 * Invariant 2: every protocol implementation (proto/<name>.c) must not
 * #include any scripting header (lua.h, quickjs.h, ...). Protocol behaviour
 * is exposed to scripts only through per-engine glue modules.
 *
 * Type definitions (proto_status, connection, protocol) are canonical in
 * include/wrkx_extension.h and re-exported here so internal code continues
 * to use #include "proto/proto.h" unchanged.
 */

#include "wrkx_extension.h"

#endif /* PROTO_H */
