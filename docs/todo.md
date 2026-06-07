TODO wrkx

Tasks:
• t077 [done] fix: retry failed initial connections instead of abandoning them forever

  Root cause: delayed_initial_connect() returned AE_NOMORE unconditionally, so any
  connection whose connect() call failed at startup sat idle for the entire run.
  With -c400 and a tight listen backlog this caused ~153 dead connections, dropping
  measured RPS from ~10 000 to ~6 000 and generating ~600 false timeouts (check_timeouts
  fired against connections whose c->start == 0 because they never sent a request).

  Changes applied to src/ and baseline/src/:

  connect_socket(): harden fd setup
    - check socket() return; goto error on -1 (was: proceeded with fd=-1)
    - check fcntl(F_GETFL) return; goto error on -1
      (was: passed garbage flags to F_SETFL, left socket blocking)
    - check fcntl(F_SETFL) return; goto error on -1
    - guard close(fd) with fd >= 0
    - save errno around the error path so callers can inspect the cause
      after close() might overwrite it

  delayed_initial_connect(): retry selected TCP-level failures
    - set c->thread_start once (first call only) so per-connection pacing
      and latency baseline reflect first scheduled time, not eventual
      successful connect time; missed slots surface as real latency pressure
    - on failure: undo connect_socket()'s errors.connect increment if this
      connection was already counted, then branch on errno
    - retryable (reschedule after INITIAL_CONNECT_RETRY_MS = 100 ms):
        ECONNREFUSED  server sent RST; listen backlog full or port closed
        ETIMEDOUT     SYN dropped; backlog full and no RST sent (Linux default)
        ENETUNREACH   routing failure; may recover
        EHOSTUNREACH  host unreachable; may recover
        ECONNRESET    connection reset after partial handshake
    - non-retryable (AE_NOMORE, connection abandoned):
        EMFILE/ENFILE fd table exhausted; retrying at 100 ms/conn is futile
        fcntl errors  kernel internal failure; not a transient condition
        ae errors     event loop at capacity; not recoverable by retrying
    - errors.connect at run end = number of unique connections that failed
      initial connect (regardless of cause or retry depth), not attempt count

  Validation
  ----------
  Server with explicit listen backlog of 16:

    nginx.conf:
      events { worker_connections 1024; }
      http { server { listen 8080 backlog=16; location / { return 200 "ok"; } } }

    Or Python (no dependencies):
      python3 -c "
      import socket, threading
      s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
      s.bind(('', 8080)); s.listen(16)
      def h(c):
          c.recv(4096)
          c.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'); c.close()
      while True: threading.Thread(target=h, args=(s.accept()[0],), daemon=True).start()
      "

  System backlog ceiling (must be >= 16 or the kernel silently caps it):
    Linux:  sysctl -w net.core.somaxconn=128 net.ipv4.tcp_max_syn_backlog=128
    macOS:  sudo sysctl -w kern.ipc.somaxconn=128

  Before patch (baseline/wrkx0):
    ./wrkx -t4 -c400 -d10s -R10000 http://127.0.0.1:8080
    ~5900-6200 RPS, errors.connect ~150, timeout ~600

  After patch:
    Same command
    ~9800-10000 RPS, errors.connect = N unique connections that hit backlog=16
    (reduce backlog further or increase -c to force more initial failures)

Bugs:
- strange behavoir on large connection parameters
./wrkx -c8 -t8 -d20 -L -R10000  http://localhost
will make ~10000 requests per second, as requested
./wrkx -c500 -t8 -d20 -L -R10000  http://localhost
will make ~4800 requests per second
./baseline/wrkx0 -c500 -t8 -d20 -L -R10000  http://localhost
will also make ~4800 requests per second but this is strange as -c500 must be cheap and lead to 10000 RPS as well. 
Investigate the issue
- baseline maybe restore 'main' or 'pre-phase0' version and build again?
- provide the nginx.config maybe that's the promlem. 

Backlog:
• REDIS and other: basic parameters, methods get/set, key ranges, key size
• output beautify, separate sections, animation like in npm/brew, configuration: ./configure string "as is" + extensions enabled, input: write back all params, results: calibration, latency summary, latency spectrum, final summary with important notes (please pay attention to + next steps)
• latency vs u-latency summary and notes
• pseudo-graphical graph-style representation of latency spectrum
• more debug-like counters like reconnects, keepalive or not keepalive, ae/even engine counters (events)
• json-like output to use in automation
• human-readable summary on anomalies
• other areas of improvements: performance, more protocol extensions, CI improvements. Gates A-? - agent wanted to refactor this piece. 
