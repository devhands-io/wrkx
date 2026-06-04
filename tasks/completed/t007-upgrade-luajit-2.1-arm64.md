title: Upgrade LuaJIT to 2.1 (ARM64 support)
status: completed
depends: []

Steps:
- rm -rf deps/luajit
- git clone --depth 1 -b v2.1 https://github.com/LuaJIT/LuaJIT deps/luajit
- make

Acceptance:
- `make` completes without errors
- `./wrk --version` runs successfully
- `make test-e2e` runs the smoke test (no longer skips)
