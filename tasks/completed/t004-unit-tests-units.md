title: Unit tests for units.c
status: completed
depends: t2

Steps:
- tests/unit/test_units.c
- Test cases:
    parse_size("1k")  == 1000
    parse_size("1M")  == 1000000
    parse_time("2s")  == 2000000  (microseconds)
    parse_time("2m")  == 120000000
    parse_time("bad") == -1 (error)

Acceptance:
- `make test-unit` passes, 0 failures
