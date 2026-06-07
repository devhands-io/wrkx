/*
 * memcached protocol implementation — skeleton for ADR 0005 Phase 4.
 *
 * t058 only registers a second extension and protocol vtable. Real text
 * protocol framing, networking, and scripting bindings arrive in later tasks.
 */

#include "memcached.h"

static int memcached_connect(connection *c) {
    (void)c;
    return -1;
}

static int memcached_write(connection *c, const char *buf, size_t len) {
    (void)c;
    (void)buf;
    (void)len;
    return -1;
}

static proto_status memcached_readable(connection *c) {
    (void)c;
    return PROTO_ERROR;
}

static void memcached_close(connection *c) {
    (void)c;
}

static protocol g_memcached_protocol = {
    .name     = "memcached",
    .connect  = memcached_connect,
    .write    = memcached_write,
    .readable = memcached_readable,
    .close    = memcached_close,
};

protocol *memcached_protocol(void) {
    return &g_memcached_protocol;
}

