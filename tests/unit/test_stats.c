#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

void test_stats_rewind(void) {
    stats *s = stats_alloc(10);
    for (uint64_t i = 1; i <= 5; i++)
        stats_record(s, i);

    /* rewind resets limit and index but preserves min/max */
    stats_rewind(s);

    TEST_ASSERT_EQUAL_UINT64(0, s->limit);
    TEST_ASSERT_EQUAL_UINT64(0, s->index);
    /* min/max are NOT reset by rewind — only by stats_reset */
    TEST_ASSERT_EQUAL_UINT64(1, s->min);
    TEST_ASSERT_EQUAL_UINT64(5, s->max);

    stats_free(s);
}

void test_stats_mean_empty(void) {
    stats *s = stats_alloc(10);
    /* limit == 0, so stats_mean must return 0 without dividing */
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 0.0, (double)stats_mean(s));
    stats_free(s);
}

void test_stats_within_stdev_array(void) {
    stats *s = stats_alloc(10);
    for (uint64_t i = 1; i <= 10; i++)
        stats_record(s, i);

    stats_summarize(s);
    long double mean  = stats_mean(s);
    long double stdev = stats_stdev(s, mean);

    /* Values 1-10: mean=5.5, stdev~3.028; within 1 stdev => ~60% */
    long double pct = stats_within_stdev(s, mean, stdev, 1);
    TEST_ASSERT_TRUE(pct >= 40.0L && pct <= 80.0L);

    stats_free(s);
}

void test_rand64_and_sample(void) {
    tinymt64_t rng;
    tinymt64_init(&rng, 42);

    /* rand64 should always return a value in [0, n) */
    for (int i = 0; i < 20; i++) {
        uint64_t v = rand64(&rng, 10);
        TEST_ASSERT_TRUE(v < 10);
    }

    /* stats_sample draws count values from src into dst */
    stats *src = stats_alloc(100);
    for (uint64_t i = 1; i <= 100; i++)
        stats_record(src, i);

    stats *dst = stats_alloc(20);
    stats_sample(dst, &rng, 20, src);
    TEST_ASSERT_EQUAL_UINT64(20, dst->limit);

    stats_free(src);
    stats_free(dst);
}

void test_stats_histogram_path(void) {
    stats *s = stats_alloc(0); /* no array slots needed — histogram handles storage */
    int rc = hdr_init(1, 1000000LL, 3, &s->histogram);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(s->histogram);

    for (int i = 1; i <= 10; i++)
        stats_record(s, i * 100); /* 100, 200, …, 1000 */

    /* histogram-path mean and stdev */
    long double mean  = stats_mean(s);
    long double stdev = stats_stdev(s, mean);
    TEST_ASSERT_TRUE(mean  > 0.0L);
    TEST_ASSERT_TRUE(stdev > 0.0L);

    /* histogram-path percentile */
    uint64_t p50 = stats_percentile(s, 50.0L);
    TEST_ASSERT_TRUE(p50 >= 100 && p50 <= 1000);

    /* histogram-path within_stdev */
    long double pct = stats_within_stdev(s, mean, stdev, 1);
    TEST_ASSERT_TRUE(pct >= 0.0L && pct <= 100.0L);

    free(s->histogram);
    s->histogram = NULL;
    stats_free(s);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mean_stdev_known_dataset);
    RUN_TEST(test_percentile_uniform);
    RUN_TEST(test_reset_zeroes_fields);
    RUN_TEST(test_stats_rewind);
    RUN_TEST(test_stats_mean_empty);
    RUN_TEST(test_stats_within_stdev_array);
    RUN_TEST(test_rand64_and_sample);
    RUN_TEST(test_stats_histogram_path);
    return UNITY_END();
}
