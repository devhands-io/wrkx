#ifndef SCRIPTING_QUICKJS_ENGINE_H
#define SCRIPTING_QUICKJS_ENGINE_H

#include "scripting/script_api.h"

script_api *quickjs_script_api(void);

/* Engine introspection: return raw JSContext / JSValue * for glue modules
 * and unit tests.  Returns NULL if se is NULL. */
void *qjs_engine_ctx(script_engine *se);    /* JSContext * */
void *qjs_engine_global(script_engine *se); /* JSValue *   */

#endif /* SCRIPTING_QUICKJS_ENGINE_H */
