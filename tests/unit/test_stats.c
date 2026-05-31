#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "unity.h"
#include "stats.h"

void setUp(void) {}
void tearDown(void) {}

void test_mean_stdev_known_dataset(void) {
    stats *s = stats_alloc(10);
    for (uint64_t i = 1; i <= 10; i++)
        stats_record(s, i);

    long double mean = stats_mean(s);

    TEST_ASSERT_DOUBLE_WITHIN(0.001, 5.5,   (double)mean);
    TEST_ASSERT_DOUBLE_WITHIN(0.01,  3.028, (double)stats_stdev(s, mean));

    stats_free(s);
}

void test_percentile_uniform(void) {
    stats *s = stats_alloc(100);
    for (uint64_t i = 1; i <= 100; i++)
        stats_record(s, i);

    stats_summarize(s);

    TEST_ASSERT_EQUAL_UINT64(51,  stats_percentile(s, 50.0L));
    TEST_ASSERT_EQUAL_UINT64(100, stats_percentile(s, 99.0L));

    stats_free(s);
}

void test_reset_zeroes_fields(void) {
    stats *s = stats_alloc(10);
    stats_record(s, 42);
    stats_record(s, 100);

    stats_reset(s);

    TEST_ASSERT_EQUAL_UINT64(0,          s->limit);
    TEST_ASSERT_EQUAL_UINT64(0,          s->index);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, s->min);
    TEST_ASSERT_EQUAL_UINT64(0,          s->max);

    stats_free(s);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mean_stdev_known_dataset);
    RUN_TEST(test_percentile_uniform);
    RUN_TEST(test_reset_zeroes_fields);
    return UNITY_END();
}
