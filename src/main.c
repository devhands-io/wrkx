/*
 * src/main.c — wrkx entry point (ADR 0001 Phase 1, P1-5).
 *
 * Wires the three layers together:
 *   1. cli.c  — parse argv → orchestrator_cfg + url + script + headers
 *   2. http1_configure()  — resolve target + supply connect info (ADR 0002 §2)
 *   3. api->configure()   — supply URL/headers to Lua engine  (ADR 0002 §3)
 *   4. api->init()        — run wrk.init(); set up default request closure
 *   5. orchestrator_create/run/collect/destroy
 *
 * This is the only file that legitimately includes headers from all three
 * layers (it is the wiring, not a layer).  wrk.c's main() remains in the
 * legacy build until the Migration Map is fully checked off.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "config.h"
#include "cli.h"
#include "orchestrator.h"
#include "proto/http1.h"
#include "scripting/lua/engine.h"
#include "units.h"
#include "http_parser.h"

/* ssl_init() from ssl.c — declared directly to avoid ssl.h → net.h → wrk.h */
extern SSL_CTX *ssl_init(void);

/* -------------------------------------------------------------------------
 * URL decomposition helper (local to wiring)
 * ---------------------------------------------------------------------- */

static char *url_part(const char *url, const struct http_parser_url *p,
                      enum http_parser_url_fields f) {
    if (!(p->field_set & (1 << f))) return NULL;
    uint16_t off = p->field_data[f].off;
    uint16_t len = p->field_data[f].len;
    char *s = calloc(len + 1, 1);
    if (s) memcpy(s, url + off, len);
    return s;
}

/*
 * Pick the first resolved address that actually accepts a connection, mirroring
 * wrk2's wrk.connect() probe. getaddrinfo() with AF_UNSPEC may return an IPv6
 * (::1) address first on a dual-stack host while the server only listens on IPv4
 * (or vice versa); using the head blindly then fails every connect — on Linux
 * `localhost` resolves to ::1 first, so an IPv4-only server is unreachable.
 * Returns a node within `list` (caller still frees the whole list); falls back
 * to the head if none probe-connect.
 */
static struct addrinfo *pick_reachable(struct addrinfo *list) {
    for (struct addrinfo *a = list; a != NULL; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int ok = (connect(fd, a->ai_addr, a->ai_addrlen) == 0);
        close(fd);
        if (ok) return a;
    }
    return list;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* ------------------------------------------------------------------
     * 1.  Parse CLI arguments.
     * ---------------------------------------------------------------- */
    char *header_buf[CLI_MAX_HEADERS];
    cli_args args;
    args.headers = header_buf;

    if (cli_parse_args(argc, argv, &args) != 0) {
        cli_usage(argv[0]);
        return 1;
    }

    /* ------------------------------------------------------------------
     * 2.  Decompose URL and resolve the connect target.
     * ---------------------------------------------------------------- */
    struct http_parser_url parts;
    memset(&parts, 0, sizeof(parts));
    http_parser_parse_url(args.url, strlen(args.url), 0, &parts);

    char *schema  = url_part(args.url, &parts, UF_SCHEMA);
    char *host    = url_part(args.url, &parts, UF_HOST);
    char *port    = url_part(args.url, &parts, UF_PORT);
    char *service = (port != NULL) ? port : schema;

    /* TLS if scheme is "https" */
    SSL_CTX *ssl_ctx = NULL;
    if (schema != NULL && strncmp("https", schema, 5) == 0) {
        ssl_ctx = ssl_init();
        if (ssl_ctx == NULL) {
            fprintf(stderr, "unable to initialise SSL\n");
            ERR_print_errors_fp(stderr);
            return 1;
        }
    }

    /* Resolve host:service → addrinfo */
    struct addrinfo hints, *addr = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, service, &hints, &addr);
    if (rc != 0) {
        fprintf(stderr, "unable to resolve %s:%s — %s\n",
                host ? host : "(null)", service ? service : "(null)",
                gai_strerror(rc));
        return 1;
    }

    /* ------------------------------------------------------------------
     * 3.  Configure the Protocol Engine (ADR 0002 Decision 2).
     *     http1_configure() is called once before orchestrator_run().
     *     The orchestrator never calls it.
     * ---------------------------------------------------------------- */
    signal(SIGPIPE, SIG_IGN);
    http1_configure(pick_reachable(addr), ssl_ctx, host);

    /* ------------------------------------------------------------------
     * 4.  Build the scripting engine and configure it (ADR 0002 §3).
     * ---------------------------------------------------------------- */
    script_api *api    = lua_script_api();
    script_engine *eng = api->create(args.script);
    if (eng == NULL) {
        fprintf(stderr, "failed to create scripting engine\n");
        return 1;
    }

    if (api->configure) {
        api->configure(eng, args.url,
                       (const char * const *) args.headers, (size_t) args.n_headers);
    }

    /* init sets up wrk.request closure and calls setup()/init() hooks */
    api->init(eng, 0, args.cfg.connections);

    /* ------------------------------------------------------------------
     * 5.  Run.
     * ---------------------------------------------------------------- */
    char *runtime_msg = format_time_s(args.cfg.duration_us / 1000000ULL);
    printf("Running %s test @ %s\n", runtime_msg, args.url);
    printf("  %"PRIu64" threads and %"PRIu64" connections\n",
           args.cfg.threads, args.cfg.connections);
    free(runtime_msg);

    args.cfg.latency           = args.latency;
    args.cfg.latency_dist_only = args.latency_dist_only;
    args.cfg.u_latency         = args.u_latency;

    orchestrator *o = orchestrator_create(args.cfg, http1_protocol(), api, eng);
    if (o == NULL) {
        fprintf(stderr, "failed to create orchestrator\n");
        return 1;
    }

    rc = orchestrator_run(o);

    orchestrator_destroy(o);

    /* Cleanup */
    freeaddrinfo(addr);
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    free(schema);
    free(host);
    free(port);

    return rc == 0 ? 0 : 1;
}
