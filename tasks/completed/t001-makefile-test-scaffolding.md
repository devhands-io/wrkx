title: Makefile test scaffolding (remove .travis, add test targets)
status: completed
depends: []

Steps:
- Delete .travis.yml
- Add to Makefile:

    test: test-unit test-e2e
    test-unit:
        @echo "no unit tests yet" && exit 0
    test-e2e:
        @echo "no e2e tests yet" && exit 0
    test-asan:
        @echo "no asan tests yet" && exit 0

Acceptance:
- `make test` exits 0
- .travis.yml is gone
