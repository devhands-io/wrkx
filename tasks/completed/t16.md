title: ASAN + UBSan unit tests
status: completed
depends: t3, t4, t5

Context:
- The full wrkx binary cannot be ASAN-instrumented because LuaJIT uses custom
  mmap-based memory management that conflicts with ASAN's shadow memory, producing
  false positives or crashes on both macOS and Linux.
- Unit test binaries have no LuaJIT dependency and can be fully instrumented.
- zmalloc.c paths exercised by unit tests are the same paths used in production;
  ASAN will catch double-frees and out-of-bounds there.

Steps:
- Add to Makefile below the coverage target:

    ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

    test-asan: | $(ODIR)
        @echo "Building ASAN unit test binaries..."
        @$(CC) $(CFLAGS) $(ASANFLAGS) $(UNITY_INC) -Isrc -DUNITY_INCLUDE_DOUBLE \
            -o obj/asan_stats \
            $(TEST_STATS_SRC) $(UNITY_SRC) $(STATS_DEPS) -lm -lpthread
        @$(CC) $(CFLAGS) $(ASANFLAGS) $(UNITY_INC) -Isrc \
            -include tests/unit/platform_compat.h \
            -o obj/asan_units \
            $(TEST_UNITS_SRC) $(UNITY_SRC) $(UNITS_DEPS) -lpthread
        @$(CC) $(CFLAGS) $(ASANFLAGS) $(UNITY_INC) -Isrc \
            -o obj/asan_hdr \
            $(TEST_HDR_SRC) $(UNITY_SRC) $(HDR_DEPS) -lm
        @echo "Running ASAN unit tests..."
        @./obj/asan_stats
        @./obj/asan_units
        @./obj/asan_hdr
        @echo "Running smoke E2E against release binary..."
        @bash tests/e2e/smoke.sh

- Add test-asan to .PHONY
- Confirm flags work on both macOS (Apple Clang) and Linux (GCC/Clang):
    - macOS: ASAN is built into the toolchain; no extra install needed
    - Linux: may need libasan (gcc) or clang with compiler-rt

Acceptance:
- `make test-asan` exits 0 with no sanitizer errors or warnings
- Any malloc/free bug in zmalloc.c surfaces here
- Running twice in a row succeeds (no stale state)
- Verified on macOS; note any Linux-specific ASAN_OPTIONS needed in comments
