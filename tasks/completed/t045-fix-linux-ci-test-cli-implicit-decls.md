title: Fix Linux CI build break — test_cli.c implicit POSIX declarations
status: completed
adr: 0001

## Symptom

GitHub Actions "Build & Test (Linux)" failed at the "Unit & E2E tests" step on
every commit since `dfa25cf5` (which added test_cli.c). macOS passed. Reported
as a "core dumped" build, but the downloaded job log shows it is actually a
**compile error**, not a runtime crash — the build never produced a binary to
crash:

```
tests/unit/test_cli.c:173: error: implicit declaration of function ‘dup’   [-Werror=implicit-function-declaration]
tests/unit/test_cli.c:174: error: implicit declaration of function ‘dup2’
tests/unit/test_cli.c:175: error: implicit declaration of function ‘close’
tests/unit/test_cli.c:189: error: implicit declaration of function ‘unlink’
tests/unit/test_cli.c:187: error: ignoring return value of ‘fread’ ...      [-Werror=unused-result]
make: *** [Makefile:163: obj/test_cli] Error 1
```

## Root cause

`test_usage_mentions_l_flag` redirects stderr using `dup`, `dup2`, `close`, and
`unlink` but never `#include <unistd.h>`. On macOS/clang those POSIX prototypes
are pulled in transitively by libc headers, so it compiles; on Linux/glibc they
are not, and `-Werror=implicit-function-declaration` makes each a hard error.
Additionally glibc marks `fread` `warn_unused_result`, so ignoring its return is
fatal under `-Werror=unused-result`. (macOS-vs-Linux header behaviour, plus the
clang/macOS CI runner masking it — the same class as t021/t022/t024.)

## Fix

- `#include <unistd.h>` in tests/unit/test_cli.c (dup/dup2/close/unlink).
- Capture `fread`'s return: `size_t nread = fread(...); buf[nread] = '\0';`
  (also tightens NUL-termination of the read buffer).

## Verification

- Audited all unit tests for the same pattern: only test_cli.c was affected
  (test_http1.c already includes unistd.h; test_lua_engine.c uses only mkstemp
  from <stdlib.h>). No ignored warn_unused_result returns elsewhere.
- The Linux "Build" step (which compiles the wrkx binary) was already passing,
  so no source/runtime regression — purely a test-file build fix.
- Local `make clean && make && make test` green.
- CI to confirm Linux green on push.

## Note

Investigated via the GitHub Actions job log (downloaded with the repo's git
credential). Earlier red commits in the range failed for progressively-fixed
reasons; test_cli.c has been the standing blocker since dfa25cf5.
