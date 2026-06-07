#ifndef CLI_H
#define CLI_H

/*
 * CLI argument parsing for wrkx (ADR 0001, Phase 1, P1-5).
 *
 * Translates argv into an orchestrator_cfg plus the URL string, optional
 * Lua script path, -H headers, and the -L/-U latency flags. The wiring in
 * main.c uses these to drive the three-layer engine startup sequence.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "orchestrator.h"

#define CLI_VERSION       "0.1.4"
#define CLI_MAX_HEADERS   64

typedef struct cli_args {
    orchestrator_cfg cfg;

    char    *url;           /* raw URL string (points into argv, not heap)    */
    char    *script;        /* -s: Lua script file path (NULL if omitted)     */
    char   **headers;       /* -H: array of raw "Key: value" strings          */
    int      n_headers;     /* number of valid entries in headers             */

    bool     latency;           /* -L: print HdrHistogram with full spectrum     */
    bool     latency_dist_only; /* -l: print distribution, no detailed spectrum  */
    bool     u_latency;         /* -U: print uncorrected latency distribution    */
} cli_args;

/*
 * Parse argc/argv into *out.  out->headers must point to a caller-allocated
 * array of at least CLI_MAX_HEADERS char * entries.
 *
 * Returns 0 on success, -1 on error (also prints a usage message to stderr).
 */
int  cli_parse_args(int argc, char **argv, cli_args *out);

void cli_usage(const char *program);

#endif /* CLI_H */
