/*
 * Unit tests for src/cli.c — cli_parse_args() and cli_usage().
 *
 * Guards the five regressions found in t035 (each mapped to a test below):
 *
 *   R1  -v output credits     — cannot be unit-tested (calls exit()); covered
 *                               by the E2E cli_output.sh test instead.
 *   R2  -l option dropped     — test_l_flag_sets_dist_only
 *   R3  -L lost spectrum      — test_L_flag_does_not_set_dist_only +
 *                               test_L_and_l_are_distinct
 *   R4  output section order  — covered by cli_output.sh E2E
 *   R5  progress bar absent   — covered by cli_output.sh E2E
 *
 * Additionally tests that cli_usage() mentions the -l flag in its text.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "unity.h"
#include "cli.h"

/* -------------------------------------------------------------------------
 * getopt global reset (needed between cli_parse_args calls in the same
 * process — getopt_long accumulates state in optind / optarg).
 * optreset is BSD/macOS; on Linux optind=1 is sufficient.
 * ---------------------------------------------------------------------- */
static void reset_getopt(void) {
    optind = 1;
#if defined(__APPLE__) || defined(__FreeBSD__)
    optreset = 1;
#endif
}

void setUp(void)    { reset_getopt(); }
void tearDown(void) {}

/* -------------------------------------------------------------------------
 * Helper: build a fake argv for cli_parse_args.
 * The URL must be the last argument to satisfy the positional check.
 * We always include -R10 so the "rate must be specified" guard passes.
 * ---------------------------------------------------------------------- */
#define URL "http://localhost:9999/"

/* Runs cli_parse_args on the given argv (NULL-terminated list of strings
 * after the program name) prepended with "wrkx" and followed by URL.
 * Returns the parse result code and fills *out. */
static int parse(cli_args *out, const char *args[], char *header_buf[]) {
    /* Count args */
    int n = 0;
    while (args[n]) n++;

    /* Build: ["wrkx", ...args..., URL, NULL] */
    int argc = 2 + n;
    char **argv = calloc(argc + 1, sizeof(char *));
    argv[0] = (char *)"wrkx";
    for (int i = 0; i < n; i++)
        argv[1 + i] = (char *)args[i];
    argv[1 + n] = (char *)URL;
    argv[1 + n + 1] = NULL;

    out->headers = header_buf;
    int rc = cli_parse_args(argc, argv, out);
    free(argv);
    reset_getopt();
    return rc;
}

/* -------------------------------------------------------------------------
 * R2 — -l / --l_latency must exist and set latency_dist_only=true
 * ---------------------------------------------------------------------- */

void test_l_flag_sets_dist_only(void) {
    char *hbuf[CLI_MAX_HEADERS];
    cli_args args = {0};
    const char *argv[] = { "-t1", "-c2", "-d1s", "-R10", "-l", NULL };
    int rc = parse(&args, argv, hbuf);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "cli_parse_args returned error for -l");
    TEST_ASSERT_TRUE_MESSAGE(args.latency, "-l must set latency=true");
    TEST_ASSERT_TRUE_MESSAGE(args.latency_dist_only,
                             "-l must set latency_dist_only=true");
}

void test_l_longopt_sets_dist_only(void) {
    char *hbuf[CLI_MAX_HEADERS];
    cli_args args = {0};
    const char *argv[] = { "-t1", "-c2", "-d1s", "-R10", "--l_latency", NULL };
    int rc = parse(&args, argv, hbuf);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc,
                                  "cli_parse_args returned error for --l_latency");
    TEST_ASSERT_TRUE_MESSAGE(args.latency_dist_only,
                             "--l_latency must set latency_dist_only=true");
}

/* -------------------------------------------------------------------------
 * R3 — -L must NOT set latency_dist_only (full spectrum mode)
 * ---------------------------------------------------------------------- */

void test_L_flag_does_not_set_dist_only(void) {
    char *hbuf[CLI_MAX_HEADERS];
    cli_args args = {0};
    const char *argv[] = { "-t1", "-c2", "-d1s", "-R10", "-L", NULL };
    int rc = parse(&args, argv, hbuf);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "cli_parse_args returned error for -L");
    TEST_ASSERT_TRUE_MESSAGE(args.latency, "-L must set latency=true");
    TEST_ASSERT_FALSE_MESSAGE(args.latency_dist_only,
                              "-L must NOT set latency_dist_only");
}

void test_L_and_l_are_distinct(void) {
    char *hbuf_L[CLI_MAX_HEADERS], *hbuf_l[CLI_MAX_HEADERS];
    cli_args args_L = {0}, args_l = {0};
    const char *argv_L[] = { "-t1", "-c2", "-d1s", "-R10", "-L", NULL };
    const char *argv_l[] = { "-t1", "-c2", "-d1s", "-R10", "-l", NULL };

    parse(&args_L, argv_L, hbuf_L);
    parse(&args_l, argv_l, hbuf_l);

    /* -L and -l both enable latency, but only -l sets dist_only */
    TEST_ASSERT_TRUE(args_L.latency);
    TEST_ASSERT_TRUE(args_l.latency);
    TEST_ASSERT_FALSE_MESSAGE(args_L.latency_dist_only,
                              "-L should not set latency_dist_only");
    TEST_ASSERT_TRUE_MESSAGE(args_l.latency_dist_only,
                             "-l should set latency_dist_only");
}

/* -------------------------------------------------------------------------
 * Sanity: -L and -l leave u_latency unaffected
 * ---------------------------------------------------------------------- */

void test_latency_flags_do_not_set_u_latency(void) {
    char *hbuf[CLI_MAX_HEADERS];
    cli_args args = {0};
    const char *argv[] = { "-t1", "-c2", "-d1s", "-R10", "-L", "-l", NULL };
    int rc = parse(&args, argv, hbuf);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE_MESSAGE(args.u_latency,
                              "-L/-l must not set u_latency");
}

/* -------------------------------------------------------------------------
 * Sanity: no latency flag → both fields false
 * ---------------------------------------------------------------------- */

void test_no_latency_flag_leaves_fields_false(void) {
    char *hbuf[CLI_MAX_HEADERS];
    cli_args args = {0};
    const char *argv[] = { "-t1", "-c2", "-d1s", "-R10", NULL };
    int rc = parse(&args, argv, hbuf);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(args.latency);
    TEST_ASSERT_FALSE(args.latency_dist_only);
    TEST_ASSERT_FALSE(args.u_latency);
}

/* -------------------------------------------------------------------------
 * cli_usage() must mention -l in its output text
 * ---------------------------------------------------------------------- */

void test_usage_mentions_l_flag(void) {
    /* Redirect stderr to a temp file, call cli_usage, check output. */
    char tmppath[] = "/tmp/wrkx_cli_usage_XXXXXX";
    int fd = mkstemp(tmppath);
    TEST_ASSERT_NOT_EQUAL(-1, fd);

    /* Swap stderr */
    int saved_stderr = dup(fileno(stderr));
    dup2(fd, fileno(stderr));
    close(fd);

    cli_usage("wrkx");

    fflush(stderr);
    dup2(saved_stderr, fileno(stderr));
    close(saved_stderr);

    /* Read and search */
    FILE *f = fopen(tmppath, "r");
    TEST_ASSERT_NOT_NULL(f);
    char buf[4096] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    unlink(tmppath);

    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(buf, "-l"),
        "cli_usage() output must contain \"-l\" (latency dist flag)");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(buf, "l_latency") != NULL || strstr(buf, "latency distribution") != NULL
            ? (void *)1 : NULL,
        "cli_usage() must describe the -l flag");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    UNITY_BEGIN();

    /* R2 — -l flag must exist */
    RUN_TEST(test_l_flag_sets_dist_only);
    RUN_TEST(test_l_longopt_sets_dist_only);

    /* R3 — -L vs -l are distinct */
    RUN_TEST(test_L_flag_does_not_set_dist_only);
    RUN_TEST(test_L_and_l_are_distinct);

    /* Sanity */
    RUN_TEST(test_latency_flags_do_not_set_u_latency);
    RUN_TEST(test_no_latency_flag_leaves_fields_false);

    /* Usage text */
    RUN_TEST(test_usage_mentions_l_flag);

    return UNITY_END();
}
