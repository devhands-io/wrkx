title: Fix build warnings
status: completed
depends: t9

Steps:
- Identify all warnings emitted by `make 2>&1 | grep warning`:
    - src/wrk.c: unused function 'print_stats_latency'
    - src/ssl.c: unused function 'ssl_lock'
    - src/ssl.c: unused function 'ssl_id'
- For each unused function, determine intent:
    - If it will be used in Phase 1 refactoring: add __attribute__((unused)) annotation
    - If it is truly dead code: remove it
- Add -Werror to CFLAGS so future warnings break the build
- Run `make clean && make && make test` and confirm 0 warnings and 0 errors

Acceptance:
- `make 2>&1 | grep -c warning` == 0
- `make test` still exits 0
- -Werror is active so the build stays clean going forward
