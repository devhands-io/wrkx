#ifndef MYSQL_QUICKJS_HELPERS_H
#define MYSQL_QUICKJS_HELPERS_H

/*
 * mysql_quickjs_helpers.h — MySQL QuickJS helper table.
 *
 * Gated on WRKX_HAVE_QUICKJS: when the build excludes QuickJS this header
 * compiles to nothing so init.c can include it unconditionally.
 *
 * ADR 0005, Phase 6 (P6-5).
 */

#ifdef WRKX_HAVE_QUICKJS

#include "wrkx_extension.h"   /* script_helper */
#include "quickjs.h"

/*
 * Per-call context bundle passed from the QuickJS engine trampoline.
 *
 * LAYOUT CONTRACT: this struct must be identical to qjs_helper_ctx in
 * engine.c and pg_quickjs_helpers.h.  Both files include quickjs.h,
 * so all field types resolve to the same ABI-level types.
 */
typedef struct {
    JSContext    *ctx;
    int           argc;
    JSValueConst *argv;
    JSValue       ret;
} qjs_helper_ctx;

extern const script_helper mysql_quickjs_helpers[];
extern const size_t        mysql_quickjs_helpers_count;

#endif /* WRKX_HAVE_QUICKJS */
#endif /* MYSQL_QUICKJS_HELPERS_H */
