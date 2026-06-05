/*
 * src/cli.c — Command-line argument parsing for wrkx (ADR 0001 P1-5).
 *
 * Translates the legacy wrkx CLI surface (ported from src/wrk.c's parse_args /
 * copy_url_part / longopts) into orchestrator_cfg and the companion cli_args
 * fields the wiring layer needs to configure each layer.
 *
 * This file has no dependency on any protocol or scripting header.
 */

#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <getopt.h>

#include "http_parser.h"
#include "units.h"
#include "ae.h"

/* -------------------------------------------------------------------------
 * URL parsing helper (ported from wrk.c copy_url_part)
 * ---------------------------------------------------------------------- */

/*
 * Returns a heap-allocated NUL-terminated copy of the named field from
 * the parsed URL, or NULL if the field is not present.  Caller frees.
 */
static char *copy_url_part(const char *url, const struct http_parser_url *parts,
                           enum http_parser_url_fields field) {
    if (!(parts->field_set & (1 << field))) return NULL;
    uint16_t off = parts->field_data[field].off;
    uint16_t len = parts->field_data[field].len;
    char *part   = calloc(len + 1, 1);
    if (part) memcpy(part, url + off, len);
    return part;
}

/* -------------------------------------------------------------------------
 * URL validation (ported from wrk.c / script.c script_parse_url)
 * ---------------------------------------------------------------------- */

static int parse_url(const char *url, struct http_parser_url *out) {
    memset(out, 0, sizeof(*out));
    if (http_parser_parse_url(url, strlen(url), 0, out) != 0) return 0;
    if (!(out->field_set & (1 << UF_SCHEMA))) return 0;
    if (!(out->field_set & (1 << UF_HOST)))   return 0;
    return 1;
}

/* -------------------------------------------------------------------------
 * Option table (mirrors wrk.c longopts)
 * ---------------------------------------------------------------------- */

static struct option longopts[] = {
    { "connections",   required_argument, NULL, 'c' },
    { "duration",      required_argument, NULL, 'd' },
    { "threads",       required_argument, NULL, 't' },
    { "script",        required_argument, NULL, 's' },
    { "header",        required_argument, NULL, 'H' },
    { "latency",       no_argument,       NULL, 'L' },
    { "l_latency",     no_argument,       NULL, 'l' },
    { "u_latency",     no_argument,       NULL, 'U' },
    { "timeout",       required_argument, NULL, 'T' },
    { "rate",          required_argument, NULL, 'R' },
    { "version",       no_argument,       NULL, 'v' },
    { "help",          no_argument,       NULL, 'h' },
    { NULL,            0,                 NULL,  0  },
};

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void cli_usage(const char *program) {
    fprintf(stderr,
        "Usage: %s <options> <url>                           \n"
        "  Options:                                          \n"
        "    -c, --connections <N>  Connections to keep open \n"
        "    -d, --duration    <T>  Duration of test         \n"
        "    -t, --threads     <N>  Number of threads to use \n"
        "    -s, --script      <S>  Load Lua script file     \n"
        "    -H, --header      <H>  Add header to request    \n"
        "    -L  --latency          Print latency statistics \n"
        "    -l  --l_latency        Print latency distribution\n"
        "                           (no detailed spectrum)   \n"
        "    -U  --u_latency        Print uncorrected latency statistics\n"
        "        --timeout     <T>  Socket/request timeout   \n"
        "    -v, --version          Print version details    \n"
        "    -R, --rate        <T>  work rate (throughput)   \n"
        "                           in requests/sec (total)  \n"
        "                           [Required Parameter]     \n"
        "                                                    \n"
        "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
        "  Time arguments may include a time unit (2s, 2m, 2h)\n",
        program ? program : "wrkx");
}

int cli_parse_args(int argc, char **argv, cli_args *out) {
    /* Defaults — same as legacy wrk.c */
    out->cfg.threads     = 2;
    out->cfg.connections = 10;
    out->cfg.duration_us = 10 * 1000000ULL;   /* 10 s */
    out->cfg.rate        = 0;
    out->url             = NULL;
    out->script          = NULL;
    out->n_headers       = 0;
    out->latency         = false;
    out->latency_dist_only = false;
    out->u_latency       = false;

    uint64_t timeout_ms  = 2000;   /* SOCKET_TIMEOUT_MS */
    uint64_t duration_s  = 10;
    int c;

    while ((c = getopt_long(argc, argv, "c:d:t:s:H:LlUT:R:vh?", longopts, NULL)) != -1) {
        switch (c) {
            case 'c':
                if (scan_metric(optarg, &out->cfg.connections)) {
                    fprintf(stderr, "invalid connections: %s\n", optarg);
                    return -1;
                }
                break;
            case 'd':
                if (scan_time(optarg, &duration_s)) {
                    fprintf(stderr, "invalid duration: %s\n", optarg);
                    return -1;
                }
                out->cfg.duration_us = duration_s * 1000000ULL;
                break;
            case 't':
                if (scan_metric(optarg, &out->cfg.threads)) {
                    fprintf(stderr, "invalid threads: %s\n", optarg);
                    return -1;
                }
                break;
            case 's':
                out->script = optarg;
                break;
            case 'H':
                if (out->n_headers < CLI_MAX_HEADERS)
                    out->headers[out->n_headers++] = optarg;
                break;
            case 'L':
                out->latency = true;
                break;
            case 'l':
                out->latency = true;
                out->latency_dist_only = true;
                break;
            case 'U':
                out->u_latency = true;
                break;
            case 'T':
                if (scan_time(optarg, &timeout_ms)) {
                    fprintf(stderr, "invalid timeout: %s\n", optarg);
                    return -1;
                }
                timeout_ms *= 1000;   /* scan_time returns seconds */
                break;
            case 'R':
                if (scan_metric(optarg, &out->cfg.rate)) {
                    fprintf(stderr, "invalid rate: %s\n", optarg);
                    return -1;
                }
                break;
            case 'v':
                printf("wrkx %s [%s] ", CLI_VERSION, aeGetApiName());
                printf("Credits: Will Glozer (wrk), Gil Tene (wrk2)\n");
                exit(0);
            case 'h':
            case '?':
            default:
                return -1;
        }
    }

    /* URL is the sole positional argument */
    if (optind >= argc) {
        fprintf(stderr, "missing URL\n");
        return -1;
    }

    if (!out->cfg.threads) {
        fprintf(stderr, "threads must be > 0\n");
        return -1;
    }

    if (!out->cfg.duration_us) {
        fprintf(stderr, "duration must be > 0\n");
        return -1;
    }

    if (!out->cfg.rate) {
        fprintf(stderr,
            "throughput must be specified with -R/--rate (e.g. -R1000)\n");
        return -1;
    }

    if (!out->cfg.connections || out->cfg.connections < out->cfg.threads) {
        fprintf(stderr,
            "connections (%"PRIu64") must be >= threads (%"PRIu64")\n",
            out->cfg.connections, out->cfg.threads);
        return -1;
    }

    /* Validate the URL */
    struct http_parser_url parts;
    out->url = argv[optind];
    if (!parse_url(out->url, &parts)) {
        fprintf(stderr, "invalid URL: %s\n", out->url);
        return -1;
    }

    (void) timeout_ms;   /* TODO: pass to orchestrator_cfg once the field exists */
    (void) copy_url_part; /* used by main.c, suppress unused-function warning */
    return 0;
}
