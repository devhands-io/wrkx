title: Unit tests for hdr_histogram.c
status: completed
depends: t2

Steps:
- tests/unit/test_hdr.c
- Test cases:
    hdr_record_value() then hdr_value_at_percentile(50.0)
    min/max after known set of values
    hdr_reset() produces clean state

Acceptance:
- `make test-unit` passes, 0 failures
