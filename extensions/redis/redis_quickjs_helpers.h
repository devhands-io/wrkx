#ifndef REDIS_QUICKJS_HELPERS_H
#define REDIS_QUICKJS_HELPERS_H

/*
 * Redis QuickJS helper table — internal to the redis extension.
 * (ADR 0005, Phase 5, t075)
 *
 * Gated on WRKX_HAVE_QUICKJS: when the build excludes QuickJS this header
 * compiles to nothing so init.c can include it unconditionally.
 *
 * Permitted includes: wrkx_extension.h, extension-internal headers,
 * deps/quickjs/ headers, standard library.  NO src/ headers.
 */

#ifdef WRKX_HAVE_QUICKJS

#include "wrkx_extension.h"   /* script_helper */
#include "quickjs.h"

/*
 * Per-call context bundle passed from the QuickJS engine trampoline
 * (src/scripting/quickjs/engine.c : js_helper_trampoline) to each
 * QuickJS-shaped helper function.
 *
 * LAYOUT CONTRACT: this struct must be identical to qjs_helper_ctx in
 * engine.c.  Both files include quickjs.h, so all field types resolve to
 * the same ABI-level types.  If you change either definition, change both.
 */
typedef struct {
    JSContext    *ctx;
    int           argc;
    JSValueConst *argv;   /* JS call arguments (borrowed, valid for the call) */
    JSValue       ret;    /* helper writes its return value here              */
} qjs_helper_ctx;

extern const script_helper redis_quickjs_helpers[];
extern const size_t        redis_quickjs_helpers_count;

#endif /* WRKX_HAVE_QUICKJS */
#endif /* REDIS_QUICKJS_HELPERS_H */
