#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <sys/time.h>

/*
 * Shared monotonic-ish wall-clock microsecond timestamp.
 *
 * Extracted from wrk.c (ADR 0001, Phase 1 Migration Map: `time_us` -> shared
 * util). Defined inline so any layer can use it without a link dependency and
 * without pulling in protocol/engine headers.
 */
static inline uint64_t time_us(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return ((uint64_t)t.tv_sec * 1000000) + (uint64_t)t.tv_usec;
}

#endif /* UTILS_H */
