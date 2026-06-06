# How to write a wrkx extension

Extensions add protocols and scripting helpers to wrkx without modifying any
core source files. They are statically linked into the binary at build time.

## Steps

**1. Create your extension directory:**
```
extensions/<name>/
    <name>.c        # implementation
    Makefile.ext    # build fragment
```

**2. Include only the public API header:**
```c
#include "wrkx_extension.h"
```
No other wrkx header is permitted. See [docs/extension-invariants.md](../docs/extension-invariants.md)
for the complete list of what extensions may and may not include.

**3. Implement the entry point:**
```c
void wrkx_extension_init_<name>(const wrkx_extension_api *api) {
    if (!api || api->version != WRKX_EXTENSION_API_VERSION) return;

    /* Register a protocol vtable (optional): */
    api->register_protocol(&my_protocol);

    /* Register scripting helpers (optional): */
    api->register_helpers("myns", my_helpers,
                          sizeof(my_helpers) / sizeof(my_helpers[0]));
}
```
Always check the version before using any other field.

**4. Write `Makefile.ext`:**
```makefile
EXT_SRCS     += extensions/<name>/<name>.c   # add more files as needed
EXT_INIT_FNS += wrkx_extension_init_<name>
```
`EXT_INIT_FNS` must list every active extension's entry point symbol.

**5. Build:**
```sh
make EXTENSIONS=<name>
# or multiple:
make EXTENSIONS="<name> other"
```

**6. Test:**
```sh
make EXTENSIONS=<name> test-unit
make EXTENSIONS=<name> check-extension-headers   # must show PASS
```
`check-extension-headers` verifies that your extension sources have zero
dependencies on private `src/` headers.

**7. Verify the binary:**
```sh
./wrkx -v    # should build and run normally with the extension linked in
```

## What extensions may include

- `include/wrkx_extension.h` — the entire extension API surface
- Standard C library headers
- Their own internal headers under `extensions/<name>/`
- Third-party dependency headers (e.g. a RESP codec, a TLS library)

**Do not include** anything under `src/` — see `docs/extension-invariants.md`.

## Static vs. dynamic loading

wrkx uses **static linking** for extensions. Extensions are compiled into the
binary at build time via `EXTENSIONS=`. No `dlopen`, no `.so` files, no
runtime search paths. This keeps the binary self-contained and avoids any
overhead during benchmark runs.

Dynamic loading may be added in a future ADR when there is a concrete reason
(e.g. third-party distribution). It must not require changes to
`include/wrkx_extension.h`.

## Example: the toy extension

`extensions/toy/` is the reference implementation. It registers a stub
protocol (`toy`) and one no-op scripting helper. It never establishes real
connections and exists only to validate the extension ABI.
