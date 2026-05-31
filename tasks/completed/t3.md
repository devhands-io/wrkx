title: Unit tests for stats.c
status: completed
depends: [T01]

Steps:
- tests/unit/test_stats.c
- Test cases:
    stats_update() → mean and stdev converge on known dataset
    stats_percentile() → 50th, 99th on uniform distribution
    stats_reset() → zeroes all fields

Acceptance:
- `make test-unit` passes, 0 failures
