# Extension invariants

**What extensions may include:**

- `include/wrkx_extension.h` — this is the entire extension API surface.
  Everything an extension needs to implement a protocol and register scripting
  helpers is declared here.
- Standard C library headers (`<stddef.h>`, `<stdint.h>`, `<string.h>`, etc.)
- Their own internal headers under `extensions/<name>/`
- Third-party dependency headers (e.g. LuaJIT for the Lua glue module)

**What extensions must NOT include:**

- `src/orchestrator.h`, `src/orchestrator.c` — Orchestrator internals
- `src/ae.h`, `src/ae.c` — event-loop internals
- `src/rate.h`, `src/rate.c` — rate-controller internals
- `src/transport.h`, `src/transport.c` — transport internals
- Any header under `src/scripting/lua/` or `src/scripting/` (engine internals)
- Any header under `src/proto/` (individual protocol headers are private)

Violating these rules means the extension depends on private core internals
and will fail the `make check-extension-headers` CI check (Gate C).

**Enforcement:**

The `check-extension-headers` Makefile target runs `$(CC) -M` on every
extension source file and fails if any `src/` path appears in the dependency
output. This check is part of CI.

**Why these boundaries?**

The goal is that extensions can be developed, built, and tested independently
of the wrkx core. If an extension includes `transport.h`, a change to
transport internals breaks the extension — the extension has become coupled to
the core. The public header boundary prevents this coupling.
