title: Rename binary target from wrk to wrkx
status: completed
depends: []

Steps:
- Change BIN := wrk to BIN := wrkx in Makefile
- Update all references to the binary name in:
    README.md (usage examples, install instructions)
    Any help/usage strings printed by the binary itself (wrk.c)
    Any other text files that reference the binary by name
- Fix all tests that reference the binary name "wrk":
    Search tests/unit/ and any future test files for hardcoded "wrk" strings
    Update runner, test helpers, or fixture paths that assume the binary is named wrk

Acceptance:
- `make` produces a binary named wrkx (not wrk)
- Running ./wrkx without arguments prints usage with "wrkx" in the output
- No remaining references to the old binary name in user-facing text
- `make test` passes, 0 failures
