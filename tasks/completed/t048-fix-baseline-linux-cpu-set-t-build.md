title: Fix baseline build on Linux — cpu_set_t undefined (parity step)
status: completed
adr: 0003
depends: t047

## Symptom

With t047 the Linux e2e finally passed; CI then reached the parity step
(`make parity`), which builds `baseline/wrkx0` and failed:

```
In file included from src/units.c:10:
src/units.h:16:5: error: unknown type name 'cpu_set_t'
make[1]: *** [Makefile:58: obj/units.o] Error 1
make: *** [Makefile:293: baseline] Error 2
```

The baseline had never been *built* on a Linux runner before (CI never got past
the earlier build/test failures), so this latent issue only surfaced now.

## Root cause

The baseline is the frozen phase-0 snapshot from commit `505cd14`. Its
`src/config.h` only provides a `cpu_set_t` stub for **non-Linux**:

```c
#if !defined(__linux__)
typedef struct { long __bits[1]; } cpu_set_t;
...
#endif
```

On Linux it provides nothing AND does not `#include <sched.h>` — so the real
glibc `cpu_set_t` is never pulled in, and `units.h`'s `struct aff_set { cpu_set_t
set; }` fails to compile. The fix that makes the *current* tree build on Linux
(`src/config.h` → `#include <sched.h>` under `_GNU_SOURCE`) postdates 505cd14, so
the frozen snapshot lacks it. macOS is unaffected: it uses the non-Linux stub.

## Fix (baseline tooling, not frozen src)

`baseline/src/` is immutable (MANIFEST guard), but `baseline/Makefile` is harness
tooling and may evolve. Supply the missing type from the build flags on Linux
only:

```makefile
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
CFLAGS  += -D_GNU_SOURCE -include sched.h
endif
```

`-include sched.h` forces glibc's <sched.h> (which defines `cpu_set_t` under
`_GNU_SOURCE`) ahead of the frozen `config.h`/`units.h`. No behavioural change —
affinity isn't exercised by the parity tests; this only lets the snapshot
compile. macOS keeps using config.h's own stub and is untouched.

## Verification

- macOS: `make baseline`, `make baseline-verify` (frozen integrity intact —
  only tooling changed), `make parity`, `make test` all green.
- Linux: CI to confirm the baseline now compiles and parity runs.

## Note

This is the 4th platform-asymmetry fix in this CI-green chain (t045 implicit
POSIX decls, t046 zmalloc/free, t047 ::1 probe, t048 baseline cpu_set_t). All
shared one cause: the macOS CI runner (clang + lenient libc + arm64) silently
tolerated what the Linux runner (gcc + glibc + x86_64) rejects.
