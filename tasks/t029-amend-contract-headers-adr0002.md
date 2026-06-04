title: Amend contract headers per ADR 0002
status: todo
adr: 0002
adr-step: headers
depends: t028

## Context

ADR 0002 (Accepted) closes three configuration gaps found during the P1-2/P1-3/P1-4
implementations. Two of those gaps require header surgery; the third (proto/proto.h)
requires no change. This task applies the header amendments only — pure contract
changes, zero implementation. The amended headers remain the single source of truth
for each layer's public surface.

Gap 1 fix (orchestrator.h): `orchestrator_create` gains `const script_api *` as
third parameter so the orchestrator can call all scripting hooks.

Gap 3 fix (script_api.h): a `configure` slot is added after `create`, giving the
wiring layer a channel to supply the target URL and custom headers to the engine.

## Scope

- **`src/orchestrator.h`**
  - Add `struct script_api;` forward declaration (alongside the existing
    `struct protocol;` and `struct script_engine;` forward declarations).
  - Change `orchestrator_create` to the four-parameter signature:
    ```c
    orchestrator *orchestrator_create(orchestrator_cfg,
                                      struct protocol *,
                                      const struct script_api *,
                                      struct script_engine *);
    ```
  - Update the comment block above `orchestrator_create` to reference ADR 0002.

- **`src/scripting/script_api.h`**
  - Insert the `configure` slot immediately after `create`:
    ```c
    /* NEW — ADR 0002 Decision 3.  Called once after create(), before init().
     * url   : full target URL (scheme://host:port/path).
     * headers: array of n_headers raw strings ("X-Foo: bar"); may be NULL.
     * Returns 0 on success.  May be NULL in the vtable; caller checks first. */
    int (*configure)(script_engine *, const char *url,
                     const char * const *headers, size_t n_headers);
    ```
  - Update the frozen-contract comment to reference ADR 0002.

- **`src/main.c` (P1-1 stub)**
  - Ensure the stub declaration or comment for `orchestrator_create` matches the
    new four-parameter form so `make contracts-check` still compiles cleanly.
    (No call site exists yet; this is a one-line comment/declaration update.)

## Steps

1. Edit `src/orchestrator.h`: add forward decl + update signature.
2. Edit `src/scripting/script_api.h`: insert `configure` slot + update comment.
3. Edit `src/main.c`: sync the stub to the new signature.
4. Run `make contracts-check` — must exit 0.
5. Run `make test` — all existing unit tests must still pass (they pass NULL for
   api, which the NULL-safety guards in orchestrator.c handle).

## Acceptance

- `make contracts-check` exits 0; three headers + stub `main.c` compile cleanly
  with no new warnings under `-Wall -Wextra -Werror`.
- `src/orchestrator.h` contains `struct script_api;` and the four-parameter
  `orchestrator_create`.
- `src/scripting/script_api.h` contains the `configure` slot between `create`
  and `init`.
- No cross-layer include is introduced; ADR 0001 Invariants 1–4 remain intact.
- `make test` exits 0 (existing unit tests unaffected).
