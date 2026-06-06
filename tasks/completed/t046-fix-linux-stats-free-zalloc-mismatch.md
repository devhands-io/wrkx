title: Fix Linux core dump — stats freed with free() instead of zfree()
status: completed
adr: 0001
depends: t045

## Symptom

After t045 unblocked the Linux build, the first e2e (smoke.sh) ran wrkx and it
**aborted with a core dump** on Linux only:

```
free(): invalid pointer
tests/e2e/smoke.sh: line 33:  3416 Aborted (core dumped) wrkx -t1 -c5 -d3s -R20 http://localhost:.../
make: *** [Makefile:197: test-e2e] Error 134
```

This is the "core dumped" originally reported. It was masked twice: (1) macOS
CI tolerates the bad free; (2) the t032-era bytecode build break and the t045
test_cli compile break both failed earlier, so the e2e step never ran on Linux.

## Root cause — allocator/deallocator mismatch (zcalloc vs free)

`stats_alloc()` allocates via `zcalloc()` (the redis zmalloc). On a platform
WITHOUT a malloc-size primitive (Linux/glibc), zmalloc prepends a size prefix
and returns `(char*)malloc_ptr + PREFIX_SIZE` — an INTERIOR pointer. Calling the
standard `free()` on that interior pointer is undefined; glibc detects it and
aborts with `free(): invalid pointer`.

On macOS `HAVE_MALLOC_SIZE` is defined (malloc_size), so `PREFIX_SIZE == 0` and
zcalloc returns the raw malloc pointer — `free()` happens to work. Hence the bug
is invisible on the macOS CI runner. (Verified locally: free() of an interior
pointer "survives" on macOS but is exactly what glibc rejects.)

Two `stats` objects were freed with `free()` instead of `stats_free()`/`zfree()`:
- `o->rps`        (orchestrator_destroy)
- `latency_stats` (orchestrator_run report stage)

`hdr_histogram` uses real `malloc`, so those `free()`s are correct; the Lua
engine/session use matching malloc/free. The two stats were the only mismatches
in the active wrkx path (script.c/units.c affinity use zmalloc but aren't wired
into the new binary).

## Fix

- `orchestrator_destroy`: `free(o->rps)` → `stats_free(o->rps)`.
- report stage: `free(latency_stats)` → `stats_free(latency_stats)`.
- Drive-by: the `format_binary((long double)bytes)` result in the "N requests
  in ... read" line was leaked; capture and free it.

`stats_free()` calls `zfree()`, which subtracts PREFIX_SIZE before calling the
system free — correct on both glibc and macOS.

## Verification

- Audited all zmalloc/zcalloc in the active layer/engine/proto sources: only the
  two stats objects were freed with the wrong deallocator.
- Standalone repro confirmed glibc aborts on an interior-pointer free while
  macOS tolerates it — explaining the platform split.
- Local `make test` green on macOS.
- CI to confirm Linux e2e green on push.

## Lesson

zmalloc-allocated memory must be released with zfree/stats_free, never the libc
free(). The macOS CI runner cannot catch this class of bug (PREFIX_SIZE==0);
the Linux job is the guard.
