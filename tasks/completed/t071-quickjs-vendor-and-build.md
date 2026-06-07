title: QuickJS — vendor pinned source and build system integration
status: completed
adr: 0005
adr-step: P5-2
depends: t070

## Goal

Embed a single, pinned QuickJS release into the repo and make it compile as part
of the wrkx build behind an opt-in flag.  No engine logic yet — only a verified,
reproducible toolchain.

## Pinned source (deterministic input)

Use the actively maintained **quickjs-ng** fork (Bellard upstream has no release
tags; quickjs-ng publishes signed releases and is API-compatible).

- Repository: `https://github.com/quickjs-ng/quickjs`
- **Pinned tag: `v0.10.0`**
- Tarball: `https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.10.0.tar.gz`

Vendor procedure (record exact output in `deps/quickjs/VERSION`):

```sh
curl -fsSL -o /tmp/qjs.tar.gz \
  https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.10.0.tar.gz
shasum -a 256 /tmp/qjs.tar.gz   # RECORD this hash in deps/quickjs/VERSION
tar -xzf /tmp/qjs.tar.gz -C /tmp
mkdir -p deps/quickjs
cp /tmp/quickjs-0.10.0/{quickjs,quickjs-atom,quickjs-opcode,libregexp,\
libregexp-opcode,libunicode,libunicode-table,cutils,list,quickjs-c-atomics,\
xsum}.* deps/quickjs/ 2>/dev/null || true   # copy what the tag actually ships
cp /tmp/quickjs-0.10.0/LICENSE deps/quickjs/LICENSE
```

> NOTE for implementer: the exact file set differs slightly by release. Copy the
> `.c`/`.h` sources the release's own `CMakeLists.txt` lists for the `qjs`
> library target — do not guess. Pin whatever the tag ships and record the
> tarball sha256.
>
> **Do not silently substitute a different version.** `v0.10.0` is the pinned
> input. If it is unavailable or its recorded sha256 cannot be reproduced, **stop
> and escalate** — do not auto-pick a newer patch during implementation. Updating
> the pin is a deliberate edit to *this task* (new tag + new verified hash,
> reviewed) made before any vendoring proceeds, not a runtime fallback.

`deps/quickjs/VERSION` (committed) must contain, exactly:
```
quickjs-ng v0.10.0
tarball-sha256: <hash from shasum above>
vendored: <YYYY-MM-DD>
```

## Deliverables

### 1. `deps/quickjs/` — vendored, pinned source + LICENSE + VERSION

### 2. `configure` — opt-in flag

```sh
QUICKJS_ENABLED=0
for arg in "$@"; do
  case "$arg" in --with-quickjs) QUICKJS_ENABLED=1 ;; esac
done
# emit QUICKJS_ENABLED into the generated config.mk
```

### 3. `Makefile` — conditional QuickJS objects

```makefile
ifeq ($(QUICKJS_ENABLED),1)
QJS_DIR    := deps/quickjs
QJS_SRCS   := $(wildcard $(QJS_DIR)/*.c)
QJS_OBJS   := $(QJS_SRCS:.c=.o)
QJS_CFLAGS := -I$(QJS_DIR) -DCONFIG_VERSION='"wrkx-vendored"'
# QuickJS ships -Wno-* needs; build its objects with relaxed warnings:
$(QJS_OBJS): CFLAGS += $(QJS_CFLAGS) -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -I$(QJS_DIR) -DWRKX_HAVE_QUICKJS=1
OBJS   += $(QJS_OBJS)
endif
```

### 4. `src/scripting/quickjs/engine.{c,h}` — compile-only stub

```c
/* engine.h */
#ifndef QUICKJS_ENGINE_H
#define QUICKJS_ENGINE_H
#include "scripting/script_api.h"
script_api *quickjs_script_api(void);
#endif
```

```c
/* engine.c */
#include "scripting/quickjs/engine.h"
#include "quickjs.h"
typedef struct { JSRuntime *rt; } qjs_engine;
static script_engine *qjs_create(const char *file) {
    (void)file; qjs_engine *e = calloc(1, sizeof(*e));
    e->rt = JS_NewRuntime(); return (script_engine *)e;
}
static void qjs_destroy(script_engine *se) {
    qjs_engine *e = (qjs_engine *)se;
    JS_FreeRuntime(e->rt); free(e);
}
static script_api qjs_api = {
    .name = "quickjs", .create = qjs_create, .destroy = qjs_destroy,
};
script_api *quickjs_script_api(void) { return &qjs_api; }
```

## Guards

- `make` (no flag) produces a byte-identical wrkx to pre-task (no regression)
- `./configure --with-quickjs && make` compiles clean; wrkx-owned code stays
  under `-Wall -Wextra` (only vendored objects get relaxed warnings)
- `./wrkx --help` exits 0 in both builds
- `deps/quickjs/VERSION` records tag + verified sha256

## Core engine touch

Zero.  Build system + a new `src/scripting/quickjs/` stub only.
