title: wrk.c god-file split design doc
status: completed
depends: t15

Context:
- src/wrk.c is 966 lines containing 36 top-level symbols across 6 distinct concerns.
- This task produces the authoritative split plan that Phase 1 executes against.
- No code is moved in this task — only the design document is produced.

Proposed module map (to be validated and finalised):

  src/output.c + src/output.h
    print_stats_header()
    print_units()
    print_stats()
    print_hdr_latency()
    print_stats_latency()        ← currently unused; keep for Phase 1

  src/cli.c + src/cli.h
    usage()
    longopts[]
    parse_args()

  src/connection.c + src/connection.h
    connect_socket()
    reconnect_socket()
    delayed_initial_connect()
    socket_connected()
    socket_writeable()
    socket_readable()
    header_field()               ← http_parser callback
    header_value()               ← http_parser callback
    response_body()              ← http_parser callback
    response_complete()          ← http_parser callback
    parser_settings              ← static struct, moves with callbacks

  src/rate.c + src/rate.h
    calibrate()
    check_timeouts()
    sample_rate()
    delay_request()
    usec_to_next_send()

  src/wrk.c  (residual after split)
    main()
    thread_main()
    progress_main()
    handler()                    ← signal handler
    time_us()
    copy_url_part()
    g_calibrated_threads         ← global shared with progress_main
    g_progress_done              ← global shared with progress_main
    sock                         ← static dispatch table
    stop                         ← volatile sig_atomic_t

Steps:
- Review the proposed module map above against the actual source
- Identify any symbols missing from the map (grep src/wrk.c for static/non-static)
- Note shared state that crosses module boundaries (globals, parser_settings, sock)
- Document which new headers wrk.h will need to include post-split
- Identify circular dependency risks (e.g. connection.c needs rate.c types or vice versa)
- Write the finalised map to docs/phase1-split.md
- Get the map reviewed/approved before any Phase 1 code moves

Acceptance:
- docs/phase1-split.md exists and is committed
- Every symbol currently in src/wrk.c appears in exactly one target module
- Cross-module dependencies are listed and have no cycles
- The document notes any symbols that should be made non-static before splitting
