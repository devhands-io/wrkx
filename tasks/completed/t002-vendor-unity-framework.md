title: Vendor the Unity unit-test framework into deps/unity
status: completed
depends: [T00]

Steps:
- git subtree add / copy Unity single-file (unity.c + unity.h)
  into deps/unity/
- Create tests/unit/runner.c with a placeholder test (TEST_ASSERT(1))
- Update Makefile test-unit target to build and run runner

Acceptance:
- `make test-unit` compiles and prints "1 test, 0 failures"
- `make test` still exits 0
