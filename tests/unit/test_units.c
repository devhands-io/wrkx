#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "units.h"
#include "zmalloc.h"

void setUp(void) {}
void tearDown(void) {}

void test_parse_size_k(void) {
    uint64_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, scan_metric("1k", &n));
    TEST_ASSERT_EQUAL_UINT64(1000, n);
}

void test_parse_size_M(void) {
    uint64_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, scan_metric("1M", &n));
    TEST_ASSERT_EQUAL_UINT64(1000000, n);
}

/* scan_time returns seconds; the caller multiplies by 1e6 for microseconds */
void test_parse_time_seconds(void) {
    uint64_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, scan_time("2s", &n));
    TEST_ASSERT_EQUAL_UINT64(2, n);
}

void test_parse_time_minutes(void) {
    uint64_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, scan_time("2m", &n));
    TEST_ASSERT_EQUAL_UINT64(120, n);
}

void test_parse_time_bad_input(void) {
    uint64_t n = 0;
    TEST_ASSERT_EQUAL_INT(-1, scan_time("bad", &n));
}

void test_format_metric_kilo(void) {
    char *s = format_metric(1000.0L);
    TEST_ASSERT_NOT_NULL(s);
    /* 1000 in metric units => "1.00k" */
    TEST_ASSERT_NOT_NULL(strstr(s, "k"));
    free(s);
}

void test_format_binary_kibi(void) {
    char *s = format_binary(1024.0L);
    TEST_ASSERT_NOT_NULL(s);
    /* 1024 in binary units => "1.00K" */
    TEST_ASSERT_NOT_NULL(strstr(s, "K"));
    free(s);
}

void test_format_time_us_milliseconds(void) {
    char *s = format_time_us(50000.0L); /* 50 000 µs = 50 ms */
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "ms"));
    free(s);
}

void test_format_time_us_large(void) {
    /* n >= 1 000 000 µs triggers the seconds branch inside format_time_us */
    char *s = format_time_us(2000000.0L); /* 2 s */
    TEST_ASSERT_NOT_NULL(s);
    /* Should produce something like "2.00s" */
    TEST_ASSERT_NOT_NULL(strstr(s, "s"));
    free(s);
}

void test_format_time_s_minutes(void) {
    char *s = format_time_s(90.0L); /* 90 s => ~1-2 m */
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "m"));
    free(s);
}

void test_scan_affinity_single(void) {
    struct aff_set_head *head = NULL;
    int rc = scan_affinity("0", &head);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(head);
    /* The head's first entry should be the item we parsed */
    struct aff_set *item = STAILQ_FIRST(head);
    TEST_ASSERT_NOT_NULL(item);
    /* Cleanup — use zfree: scan_affinity allocates via zcalloc/zmalloc */
    zfree(item);
    zfree(head);
}

void test_scan_affinity_multi(void) {
    struct aff_set_head *head = NULL;
    int rc = scan_affinity("0,1", &head);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(head);
    /* Walk and free the list */
    struct aff_set *cur, *next;
    cur = STAILQ_FIRST(head);
    while (cur) {
        next = STAILQ_NEXT(cur, items);
        zfree(cur);   /* allocated with zcalloc inside scan_affinity */
        cur = next;
    }
    zfree(head);      /* allocated with zmalloc inside scan_affinity */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_size_k);
    RUN_TEST(test_parse_size_M);
    RUN_TEST(test_parse_time_seconds);
    RUN_TEST(test_parse_time_minutes);
    RUN_TEST(test_parse_time_bad_input);
    RUN_TEST(test_format_metric_kilo);
    RUN_TEST(test_format_binary_kibi);
    RUN_TEST(test_format_time_us_milliseconds);
    RUN_TEST(test_format_time_us_large);
    RUN_TEST(test_format_time_s_minutes);
    RUN_TEST(test_scan_affinity_single);
    RUN_TEST(test_scan_affinity_multi);
    return UNITY_END();
}
