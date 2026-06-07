/* src/scripting/lua/engine.c
 *
 * LuaJIT Request-Layer engine (ADR 0001, Phase 1, P1-4).
 *
 * Implements the frozen script_api vtable for LuaJIT. Owns the lua_State, the
 * wrk.* Lua API surface and the init/request/response/done hook contract,
 * preserving the behaviour of the legacy src/script.c. Per-protocol helpers are
 * registered here during init via glue modules; the engine never includes a
 * protocol header (Invariant 3).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "scripting/script_api.h"
#include "scripting/lua/engine.h"
#include "scripting/lua/http1_helpers.h"
#include "orchestrator.h"
#include "http_parser.h"   /* http_parser_parse_url — ADR 0002 configure slot */

/* -------------------------------------------------------------------------
 * Engine state
 * ---------------------------------------------------------------------- */

struct helper_set {
    char              *ns;
    const script_helper *helpers;
    size_t              count;
};

struct script_engine {
    lua_State *L;
    char      *request_buf;   /* reused scratch buffer for request() */
    size_t     request_cap;
    /* replay inputs for clone() */
    char      *path;
    char      *url;
    char     **headers;
    size_t     n_headers;
    struct helper_set *helper_sets;
    size_t             n_helper_sets;
};

/* -------------------------------------------------------------------------
 * wrk.* native helpers (ported from src/script.c, thread-independent subset)
 * ---------------------------------------------------------------------- */

static int lua_wrk_time_us(lua_State *L) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = ((uint64_t) tv.tv_sec * 1000000) + tv.tv_usec;
    lua_pushnumber(L, (lua_Number) now);
    return 1;
}

/* Sets t[field] = value where t is the table on top of the stack. */
static void set_field_str(lua_State *L, const char *field, const char *value) {
    lua_pushstring(L, value);
    lua_setfield(L, -2, field);
}

/*
 * Loads the wrk module and installs the script file. The frozen
 * script_api.create() receives only a file path — no URL or headers — so the
 * wrk.* defaults from src/wrk.lua (scheme=http, host=localhost, path=/) stand
 * unless the script overrides them. (Contract gap noted in the task report.)
 */
static lua_State *engine_new_state(const char *file) {
    lua_State *L = luaL_newstate();
    if (L == NULL) return NULL;
    luaL_openlibs(L);

    if (luaL_dostring(L, "wrk = require \"wrk\"") != 0) {
        fprintf(stderr, "wrk module load failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return NULL;
    }

    /* Install the thread-independent wrk.* native functions. */
    lua_getglobal(L, "wrk");
    lua_pushcfunction(L, lua_wrk_time_us);
    lua_setfield(L, -2, "time_us");
    set_field_str(L, "path", "/");
    lua_pop(L, 1);

    if (file != NULL && luaL_dofile(L, file) != 0) {
        fprintf(stderr, "%s: %s\n", file, lua_tostring(L, -1));
        /* Match legacy script.c: report and continue with a usable state. */
        lua_pop(L, 1);
    }

    return L;
}

/* -------------------------------------------------------------------------
 * Helper registration (lua_register_helpers — engine-internal, vtable-bound)
 * ---------------------------------------------------------------------- */

/*
 * Each registered helper is a script_helper_fn that takes an opaque engine_ctx.
 * For LuaJIT engine_ctx is the lua_State *, so we wrap each helper in a Lua
 * C closure that simply forwards L. The closure stores the function pointer as
 * a light userdata upvalue.
 */
static int lua_helper_trampoline(lua_State *L) {
    script_helper_fn fn = (script_helper_fn) lua_touserdata(L, lua_upvalueindex(1));
    return fn((void *) L);
}

void lua_register_helpers(script_engine       *engine,
                          const char          *ns,
                          const script_helper *helpers,
                          size_t               count) {
    if (engine == NULL || engine->L == NULL || ns == NULL) return;
    lua_State *L = engine->L;

    /* Record for clone() replay. The helpers pointer itself is stable (static
     * table in the extension); only the namespace string needs copying. */
    struct helper_set *sets = realloc(engine->helper_sets,
                                      (engine->n_helper_sets + 1) * sizeof(*sets));
    if (sets) {
        engine->helper_sets = sets;
        sets[engine->n_helper_sets].ns      = strdup(ns);
        sets[engine->n_helper_sets].helpers = helpers;
        sets[engine->n_helper_sets].count   = count;
        engine->n_helper_sets++;
    }

    /* Namespace table: reuse an existing global of the same name, else create
     * one. Helpers land as ns.<name> = closure. */
    lua_getglobal(L, ns);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }

    for (size_t i = 0; i < count; i++) {
        if (helpers[i].name == NULL || helpers[i].fn == NULL) continue;
        lua_pushlightuserdata(L, (void *) helpers[i].fn);
        lua_pushcclosure(L, lua_helper_trampoline, 1);
        lua_setfield(L, -2, helpers[i].name);
    }

    lua_setglobal(L, ns);
}

void *lua_engine_state(script_engine *engine) {
    return engine != NULL ? engine->L : NULL;
}

/* -------------------------------------------------------------------------
 * script_api vtable
 * ---------------------------------------------------------------------- */

static script_engine *lua_create(const char *file) {
    script_engine *engine = calloc(1, sizeof(*engine));
    if (engine == NULL) return NULL;

    engine->L = engine_new_state(file);
    if (engine->L == NULL) {
        free(engine);
        return NULL;
    }
    if (file != NULL)
        engine->path = strdup(file);
    return engine;
}

/* -------------------------------------------------------------------------
 * ADR 0002 Decision 3 — configure slot
 * ---------------------------------------------------------------------- */

/*
 * lua_configure: called once after create(), before init().
 *
 * Parses `url` with http_parser_parse_url and sets:
 *   wrk.scheme, wrk.host, wrk.port (number), wrk.path
 *
 * Iterates `headers[0..n_headers-1]` and installs each "Key: value" string
 * into wrk.headers, matching the legacy script_create(file, url, headers)
 * behaviour. Either argument may be NULL / zero without error.
 *
 * Invariant 3: http_parser.h is a project-internal parser utility, not a
 * protocol implementation header. Including it here is permitted.
 */
static int lua_configure(script_engine *engine, const char *url,
                         const char * const *headers, size_t n_headers) {
    if (engine == NULL || engine->L == NULL) return -1;
    lua_State *L = engine->L;

    /* Record for clone() replay. */
    free(engine->url);
    engine->url = url ? strdup(url) : NULL;
    if (n_headers > 0 && headers != NULL) {
        for (size_t i = 0; i < engine->n_headers; i++) free(engine->headers[i]);
        free(engine->headers);
        engine->headers = calloc(n_headers, sizeof(char *));
        engine->n_headers = 0;
        if (engine->headers) {
            for (size_t i = 0; i < n_headers; i++) {
                engine->headers[i] = headers[i] ? strdup(headers[i]) : NULL;
                engine->n_headers++;
            }
        }
    }

    /* --- URL fields ---------------------------------------------------- */
    if (url != NULL) {
        struct http_parser_url u;
        memset(&u, 0, sizeof(u));
        if (http_parser_parse_url(url, strlen(url), 0, &u) == 0) {
            lua_getglobal(L, "wrk");   /* index -1 */

            if (u.field_set & (1 << UF_SCHEMA)) {
                lua_pushlstring(L, url + u.field_data[UF_SCHEMA].off,
                                u.field_data[UF_SCHEMA].len);
                lua_setfield(L, -2, "scheme");
            }
            if (u.field_set & (1 << UF_HOST)) {
                lua_pushlstring(L, url + u.field_data[UF_HOST].off,
                                u.field_data[UF_HOST].len);
                lua_setfield(L, -2, "host");
            }
            if (u.field_set & (1 << UF_PORT)) {
                /* Push the pre-converted uint16_t port as a Lua number;
                 * wrk.lua uses it in string concat ("host" .. ":" .. port)
                 * which coerces numbers transparently. */
                lua_pushnumber(L, (lua_Number) u.port);
                lua_setfield(L, -2, "port");
            }
            if (u.field_set & (1 << UF_PATH)) {
                lua_pushlstring(L, url + u.field_data[UF_PATH].off,
                                u.field_data[UF_PATH].len);
                lua_setfield(L, -2, "path");
            }

            lua_pop(L, 1); /* wrk */
        }
    }

    /* --- Custom headers ------------------------------------------------- */
    if (headers != NULL && n_headers > 0) {
        lua_getglobal(L, "wrk");         /* index -1: wrk */
        lua_getfield(L, -1, "headers");  /* index -1: wrk.headers (or nil) */
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);             /* create a fresh headers table */
            lua_pushvalue(L, -1);        /* dup so we can setfield + keep ref */
            lua_setfield(L, -3, "headers");
        }
        /* Stack: wrk (-2), headers table (-1) */
        for (size_t i = 0; i < n_headers; i++) {
            if (headers[i] == NULL) continue;
            /* Split on first ": " — same rule as legacy script_create. */
            const char *colon = strchr(headers[i], ':');
            if (colon == NULL || colon[1] != ' ') continue;
            size_t key_len = (size_t)(colon - headers[i]);
            const char *val = colon + 2;
            lua_pushlstring(L, headers[i], key_len);
            lua_pushstring(L, val);
            lua_settable(L, -3);
        }
        lua_pop(L, 2); /* headers table + wrk */
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Capability detection (ADR 0005, Phase 5, t069)
 * ---------------------------------------------------------------------- */

/*
 * Lua-specific realization of the language-neutral capability contract.
 *
 * wrk.lua installs the *static* default request as a closure on `wrk.request`
 * during wrk.init() — never as a global `request`. A script that wants a fresh
 * request per call defines a global `request` function, and lua_request()
 * prefers that global over wrk.request. So "a global `request` function exists"
 * is the precise signal that this workload is dynamic; likewise a global
 * `response` function means the script wants per-response callbacks.
 *
 * Globals are populated when create() executes the script file, so this may be
 * called any time after create() (no init() required). A NULL script defines
 * neither global and reports 0 (pure static default GET).
 */
static uint32_t lua_capabilities(script_engine *engine) {
    if (engine == NULL || engine->L == NULL) return 0;
    lua_State *L = engine->L;
    uint32_t caps = 0;

    lua_getglobal(L, "request");
    if (lua_isfunction(L, -1)) caps |= SCRIPT_CAP_DYNAMIC_REQUEST;
    lua_pop(L, 1);

    lua_getglobal(L, "response");
    if (lua_isfunction(L, -1)) caps |= SCRIPT_CAP_RESPONSE_HOOK;
    lua_pop(L, 1);

    return caps;
}

static void lua_init(script_engine *engine, uint64_t thread_id,
                     uint64_t connections) {
    (void) thread_id;
    (void) connections;
    if (engine == NULL) return;
    lua_State *L = engine->L;

    /* Register per-protocol helper namespaces via glue modules. This is the
     * only point the engine reaches protocol behaviour, and it does so through
     * a glue module — never by including a protocol header (Invariant 3/4). */
    lua_register_http1_helpers(engine);

    /* Call the user setup(thread) hook if defined, passing an empty table as
     * the thread descriptor.  In the new single-engine architecture there is
     * one Lua state shared by the main path and done(); calling setup() here
     * preserves the legacy invariant that setup_called is visible to done(). */
    lua_getglobal(L, "setup");
    if (lua_isfunction(L, -1)) {
        lua_newtable(L);   /* empty thread descriptor */
        if (lua_pcall(L, 1, 0, 0) != 0) {
            fprintf(stderr, "setup hook failed: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    /* Build the default request closure and run the user init hook, mirroring
     * src/wrk.lua's wrk.init(args). The frozen init() carries no argv, so pass
     * an empty args table. */
    lua_getglobal(L, "wrk");
    lua_getfield(L, -1, "init");
    lua_newtable(L);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        fprintf(stderr, "wrk.init failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* wrk */
}

static char *lua_request(script_engine *engine, size_t *len_out) {
    if (engine == NULL) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    lua_State *L = engine->L;

    int pop = 1;
    lua_getglobal(L, "request");
    if (!lua_isfunction(L, -1)) {
        lua_getglobal(L, "wrk");
        lua_getfield(L, -1, "request");
        pop += 2;
    }
    lua_call(L, 0, 1);

    size_t len = 0;
    const char *str = lua_tolstring(L, -1, &len);

    char *buf = malloc(len > 0 ? len : 1);
    if (buf != NULL && str != NULL) {
        memcpy(buf, str, len);
    }
    lua_pop(L, pop);

    if (len_out) *len_out = (buf != NULL) ? len : 0;
    return buf;
}

static void lua_response(script_engine *engine, int status, size_t bytes,
                         uint64_t latency_us) {
    (void) bytes;
    (void) latency_us;
    if (engine == NULL) return;
    lua_State *L = engine->L;

    lua_getglobal(L, "response");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    /* Legacy shape: response(status, headers, body). The frozen contract gives
     * only status/bytes/latency, so headers is an empty table and body empty.
     * (Contract gap noted in the task report.) */
    lua_pushinteger(L, status);
    lua_newtable(L);
    lua_pushlstring(L, "", 0);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        fprintf(stderr, "response hook failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void push_summary(lua_State *L, struct orchestrator_stats *stats) {
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number) stats->elapsed_us);
    lua_setfield(L, -2, "duration");
    lua_pushnumber(L, (lua_Number) stats->requests);
    lua_setfield(L, -2, "requests");

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number) stats->errors_connect);
    lua_setfield(L, -2, "connect");
    lua_pushnumber(L, (lua_Number) stats->errors_status);
    lua_setfield(L, -2, "status");
    lua_pushnumber(L, (lua_Number) stats->errors_timeout);
    lua_setfield(L, -2, "timeout");
    lua_setfield(L, -2, "errors");
}

static void lua_done(script_engine *engine, struct orchestrator_stats *stats) {
    if (engine == NULL || stats == NULL) return;
    lua_State *L = engine->L;

    lua_getglobal(L, "done");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    /* Legacy shape: done(summary, latency, requests). The frozen orchestrator_
     * stats does not expose the wrk.stats userdata objects, so latency/requests
     * are passed as nil. (Contract gap noted in the task report.) */
    push_summary(L, stats);
    lua_pushnil(L);
    lua_pushnil(L);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        fprintf(stderr, "done hook failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static script_engine *lua_clone(script_engine *src) {
    if (src == NULL) return NULL;
    script_engine *e = lua_create(src->path);          /* step 1 (NULL-safe) */
    if (e == NULL) return NULL;
    for (size_t i = 0; i < src->n_helper_sets; i++)    /* step 2 */
        lua_register_helpers(e, src->helper_sets[i].ns,
                             src->helper_sets[i].helpers,
                             src->helper_sets[i].count);
    if (src->url || src->n_headers)                    /* step 3 */
        lua_configure(e, src->url,
                      (const char * const *) src->headers, src->n_headers);
    return e;
}

static void lua_destroy(script_engine *engine) {
    if (engine == NULL) return;
    if (engine->L != NULL) lua_close(engine->L);
    free(engine->request_buf);
    free(engine->path);
    free(engine->url);
    for (size_t i = 0; i < engine->n_headers; i++) free(engine->headers[i]);
    free(engine->headers);
    for (size_t i = 0; i < engine->n_helper_sets; i++)
        free(engine->helper_sets[i].ns);
    free(engine->helper_sets);
    free(engine);
}

static script_api lua_api = {
    .name             = "lua",
    .create           = lua_create,
    .configure        = lua_configure,   /* ADR 0002 Decision 3 */
    .capabilities     = lua_capabilities,/* ADR 0005 Phase 5 t069 */
    .register_helpers = lua_register_helpers,
    .clone            = lua_clone,       /* ADR 0005 Phase 5 t070 */
    .init             = lua_init,
    .request          = lua_request,
    .response         = lua_response,
    .done             = lua_done,
    .destroy          = lua_destroy,
};

script_api *lua_script_api(void) {
    return &lua_api;
}
