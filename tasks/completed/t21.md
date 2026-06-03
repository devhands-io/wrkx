title: Fix Linux CI — missing sys/time.h in script.c
status: completed
depends: t19

Context:
- src/script.c uses struct timeval and gettimeofday() in script_wrk_time_us()
  (line ~476) but does not include <sys/time.h>.
- On macOS, <sys/time.h> leaks in transitively through LuaJIT headers, so the
  build succeeds silently.
- On Linux with GCC and -std=c99, <sys/time.h> is not pulled in automatically;
  gettimeofday() is undeclared, producing -Werror=implicit-function-declaration.

Steps:
- Add #include <sys/time.h> to src/script.c after the existing system includes.
- Run `make clean && make && make test` locally (macOS) — must exit 0, 0 warnings.
- Push; Linux CI job must go green.

Acceptance:
- `make 2>&1 | grep -c warning` == 0 on macOS
- Linux CI job passes with no implicit-function-declaration errors
