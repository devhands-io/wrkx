title: Fix Linux CI — cpu_set_t undeclared in units.h/units.c
status: completed
depends: t21

Context:
- src/config.h guards cpu_set_t with #if !defined(__linux__), providing a
  no-op stub on macOS/FreeBSD. The intent was that Linux uses the real type.
- However the Linux branch has no #include <sched.h>, so cpu_set_t, CPU_ZERO
  and CPU_SET are all undeclared when compiling units.c on Linux, producing
  three -Werror errors.
- <sched.h> is the POSIX header that provides cpu_set_t, CPU_ZERO, CPU_SET
  on Linux (visible with -D_GNU_SOURCE which configure already adds).

Fix — one change in src/config.h:
  Change:
      #if !defined(__linux__)
      #include <string.h>
      typedef struct { long __bits[1]; } cpu_set_t;
      #define CPU_ZERO(s)    memset((s), 0, sizeof(*(s)))
      #define CPU_SET(n, s)  ((void)(n))
      #endif

  To:
      #if !defined(__linux__)
      #include <string.h>
      typedef struct { long __bits[1]; } cpu_set_t;
      #define CPU_ZERO(s)    memset((s), 0, sizeof(*(s)))
      #define CPU_SET(n, s)  ((void)(n))
      #else
      #include <sched.h>  /* cpu_set_t, CPU_ZERO, CPU_SET */
      #endif

Steps:
- Apply the change to src/config.h
- Run `make clean && make && make test` locally (macOS) — must exit 0, 0 warnings
- Push; Linux CI job must go green

Acceptance:
- `make 2>&1 | grep -c warning` == 0 on macOS
- Linux CI job passes with no cpu_set_t / CPU_ZERO / CPU_SET errors
