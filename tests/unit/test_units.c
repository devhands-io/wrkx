#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "unity.h"
#include "units.h"

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_size_k);
    RUN_TEST(test_parse_size_M);
    RUN_TEST(test_parse_time_seconds);
    RUN_TEST(test_parse_time_minutes);
    RUN_TEST(test_parse_time_bad_input);
    return UNITY_END();
}
