#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

/* cpu_set_t and friends are Linux-only; provide no-op stubs on other platforms */
#if !defined(__linux__)
typedef int cpu_set_t;
#define CPU_ZERO(s)    ((void)(s))
#define CPU_SET(n, s)  ((void)(n))
#endif

#endif /* PLATFORM_COMPAT_H */
