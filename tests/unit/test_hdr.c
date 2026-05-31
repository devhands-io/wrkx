#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"
#include "hdr_histogram.h"

static struct hdr_histogram *h;

void setUp(void) {
    hdr_init(1, 1000000, 3, &h);
}

void tearDown(void) {
    free(h);
    h = NULL;
}

void test_percentile_50(void) {
    for (int64_t i = 1; i <= 100; i++)
        hdr_record_value(h, i);

    int64_t p50 = hdr_value_at_percentile(h, 50.0);
    TEST_ASSERT_INT64_WITHIN(2, 50, p50);
}

void test_min_max_known_values(void) {
    hdr_record_value(h, 10);
    hdr_record_value(h, 500);
    hdr_record_value(h, 100);

    TEST_ASSERT_EQUAL_INT64(10,  hdr_min(h));
    TEST_ASSERT_EQUAL_INT64(500, hdr_max(h));
}

void test_reset_clean_state(void) {
    hdr_record_value(h, 42);
    hdr_record_value(h, 999);

    hdr_reset(h);

    TEST_ASSERT_EQUAL_INT64(0, h->total_count);
    TEST_ASSERT_EQUAL_INT64(0, hdr_min(h));
    TEST_ASSERT_EQUAL_INT64(0, hdr_max(h));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_percentile_50);
    RUN_TEST(test_min_max_known_values);
    RUN_TEST(test_reset_clean_state);
    return UNITY_END();
}
