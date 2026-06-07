/* src/scripting/quickjs/engine.c
 *
 * QuickJS Request-Layer engine stub (ADR 0005, Phase 5, t071).
 *
 * Compile-only skeleton: verifies the build toolchain links QuickJS and
 * exposes the script_api vtable entry point. Full implementation follows
 * in t072-t075.
 */

#include <stdlib.h>

#include "scripting/quickjs/engine.h"
#include "quickjs.h"

typedef struct {
    JSRuntime *rt;
} qjs_engine;

static script_engine *qjs_create(const char *file) {
    (void)file;
    qjs_engine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->rt = JS_NewRuntime();
    return (script_engine *) e;
}

static void qjs_destroy(script_engine *se) {
    if (!se) return;
    qjs_engine *e = (qjs_engine *) se;
    JS_FreeRuntime(e->rt);
    free(e);
}

static script_api qjs_api = {
    .name    = "quickjs",
    .create  = qjs_create,
    .destroy = qjs_destroy,
};

script_api *quickjs_script_api(void) {
    return &qjs_api;
}
