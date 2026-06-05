#ifndef CONFIG_H
#define CONFIG_H

#if defined(__FreeBSD__) || defined(__APPLE__)
#define HAVE_KQUEUE
#elif defined(__linux__)
#define HAVE_EPOLL
#define _POSIX_C_SOURCE 200809L
#elif defined (__sun)
#define HAVE_EVPORT
#endif

/* cpu_set_t, CPU_ZERO, CPU_SET:
 * - Linux:   real types from <sched.h> (visible with -D_GNU_SOURCE)
 * - Others:  no-op stubs so non-Linux builds compile without sched.h */
#if defined(__linux__)
#include <sched.h>
#else
#include <string.h>
typedef struct { long __bits[1]; } cpu_set_t;
#define CPU_ZERO(s)    memset((s), 0, sizeof(*(s)))
#define CPU_SET(n, s)  ((void)(n))
#endif

#endif /* CONFIG_H */
