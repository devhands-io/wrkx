title: Fix bytecode.o missing libluajit.a prerequisite (CI build failure)
status: todo
adr: —
depends: —

## Context

CI (both macOS and Linux) fails on `make clean && make` with:

```
LUAJIT src/wrk.lua
./luajit: No such file or directory
make: *** [obj/bytecode.o] Error 127
```

Root cause: the `$(ODIR)/bytecode.o` Makefile rule compiles `src/wrk.lua` to
LuaJIT bytecode by running `./luajit` inside `$(LDIR)`. However, the rule
declares only `src/wrk.lua` as a prerequisite — it does not depend on
`$(LDIR)/libluajit.a`. So after `make clean` (which removes `deps/luajit/src/luajit`),
make cannot guarantee the LuaJIT binary is built before the bytecode step runs.

On a warm dev machine the binary already exists, masking the bug. On a fresh CI
runner — or any `clean && make` — it surfaces.

## Fix

Add `$(LDIR)/libluajit.a` and `| $(ODIR)` to the prerequisites of the
`$(ODIR)/bytecode.o` rule in `Makefile`:

```makefile
# Before
$(ODIR)/bytecode.o: src/wrk.lua
	@echo LUAJIT $<
	@$(SHELL) -c 'cd $(LDIR) && ./luajit -b $(CURDIR)/$< $(CURDIR)/$@'

# After
$(ODIR)/bytecode.o: src/wrk.lua $(LDIR)/libluajit.a | $(ODIR)
	@echo LUAJIT $<
	@$(SHELL) -c 'cd $(LDIR) && ./luajit -b $(CURDIR)/$< $(CURDIR)/$@'
```

`$(LDIR)/libluajit.a` already has its own rule that builds the full LuaJIT
tree (including the `luajit` binary). Making it a normal (not order-only)
prerequisite ensures the bytecode rule reruns whenever libluajit.a is rebuilt.

## Steps

1. Edit `Makefile`: add `$(LDIR)/libluajit.a | $(ODIR)` to the
   `$(ODIR)/bytecode.o` prerequisite list.
2. Run `make clean && make` locally and confirm the build succeeds.
3. Verify `make test` is still green.

## Acceptance

- `make clean && make` succeeds from a fully clean state (no pre-existing
  `deps/luajit/src/luajit`).
- `make test` exits 0; all unit + E2E tests pass.
- CI macOS and Linux jobs both pass on the next push.
