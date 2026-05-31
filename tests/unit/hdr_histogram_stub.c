#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "hdr_histogram.h"

/* Stubs — histogram path is never reached in unit tests (histogram == NULL). */
int    hdr_init(int64_t l, int64_t h, int sf, struct hdr_histogram **r)          { (void)l;(void)h;(void)sf;(void)r; return 0; }
int    hdr_alloc(int64_t h, int sf, struct hdr_histogram **r)                     { (void)h;(void)sf;(void)r; return 0; }
void   hdr_reset(struct hdr_histogram *h)                                          { (void)h; }
size_t hdr_get_memory_size(struct hdr_histogram *h)                                { (void)h; return 0; }
bool   hdr_record_value(struct hdr_histogram *h, int64_t v)                        { (void)h;(void)v; return false; }
bool   hdr_record_values(struct hdr_histogram *h, int64_t v, int64_t c)           { (void)h;(void)v;(void)c; return false; }
bool   hdr_record_corrected_value(struct hdr_histogram *h, int64_t v, int64_t ei) { (void)h;(void)v;(void)ei; return false; }
int64_t hdr_add(struct hdr_histogram *h, struct hdr_histogram *f)                  { (void)h;(void)f; return 0; }
int64_t hdr_min(struct hdr_histogram *h)                                            { (void)h; return 0; }
int64_t hdr_max(struct hdr_histogram *h)                                            { (void)h; return 0; }
int64_t hdr_value_at_percentile(struct hdr_histogram *h, double p)                 { (void)h;(void)p; return 0; }
double  hdr_mean(struct hdr_histogram *h)                                           { (void)h; return 0.0; }
double  hdr_stddev(struct hdr_histogram *h)                                         { (void)h; return 0.0; }
bool    hdr_values_are_equivalent(struct hdr_histogram *h, int64_t a, int64_t b)   { (void)h;(void)a;(void)b; return false; }
int64_t hdr_lowest_equivalent_value(struct hdr_histogram *h, int64_t v)            { (void)h;(void)v; return 0; }
int64_t hdr_count_at_value(struct hdr_histogram *h, int64_t v)                     { (void)h;(void)v; return 0; }
void    hdr_iter_init(struct hdr_iter *it, struct hdr_histogram *h)                 { (void)it;(void)h; }
bool    hdr_iter_next(struct hdr_iter *it)                                          { (void)it; return false; }
void    hdr_percentile_iter_init(struct hdr_percentile_iter *p, struct hdr_histogram *h, int32_t t) { (void)p;(void)h;(void)t; }
bool    hdr_percentile_iter_next(struct hdr_percentile_iter *p)                     { (void)p; return false; }
int     hdr_percentiles_print(struct hdr_histogram *h, FILE *s, int32_t t, double vs, format_type f) { (void)h;(void)s;(void)t;(void)vs;(void)f; return 0; }
void    hdr_recorded_iter_init(struct hdr_recorded_iter *r, struct hdr_histogram *h) { (void)r;(void)h; }
bool    hdr_recorded_iter_next(struct hdr_recorded_iter *r)                          { (void)r; return false; }
void    hdr_linear_iter_init(struct hdr_linear_iter *l, struct hdr_histogram *h, int v) { (void)l;(void)h;(void)v; }
bool    hdr_linear_iter_next(struct hdr_linear_iter *l)                              { (void)l; return false; }
void    hdr_log_iter_init(struct hdr_log_iter *l, struct hdr_histogram *h, int v, double b) { (void)l;(void)h;(void)v;(void)b; }
bool    hdr_log_iter_next(struct hdr_log_iter *l)                                    { (void)l; return false; }
