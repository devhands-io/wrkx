/*
 * Toy extension (ADR 0005, Phase 3, P3-1).
 *
 * Demonstrates the extension ABI. No functional protocol — all vtable
 * operations are stubs. Compiled against include/wrkx_extension.h only;
 * no src/ headers are permitted.
 *
 * Entry point: wrkx_extension_init_toy
 */

#include "wrkx_extension.h"

/* ---- toy protocol vtable (stubs — never establishes a real connection) -- */

static int toy_connect(connection *c) { (void)c; return -1; }
static int toy_write(connection *c, const char *b, size_t n)
    { (void)c; (void)b; (void)n; return -1; }
static proto_status toy_readable(connection *c) { (void)c; return PROTO_ERROR; }
static void toy_close(connection *c) { (void)c; }

static const protocol toy_protocol = {
    .name     = "toy",
    .connect  = toy_connect,
    .write    = toy_write,
    .readable = toy_readable,
    .close    = toy_close,
};

/* ---- toy scripting helpers --------------------------------------------- */

static int toy_noop(void *ctx) { (void)ctx; return 0; }

static const script_helper toy_helpers[] = {
    { "noop", toy_noop },
};

/* ---- extension entry point --------------------------------------------- */

void wrkx_extension_init_toy(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    api->register_protocol(&toy_protocol);
    api->register_helpers("toy", toy_helpers,
                          sizeof(toy_helpers) / sizeof(toy_helpers[0]));
}
