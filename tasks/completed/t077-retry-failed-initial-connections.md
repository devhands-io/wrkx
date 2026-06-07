title: fix: retry failed initial connections instead of abandoning them forever
status: completed
commit: 21cbf7c

## Goal

Fix a performance regression where increasing `-c` beyond a threshold drops RPS
dramatically. Root cause: `delayed_initial_connect()` returned `AE_NOMORE`
unconditionally, so any connection whose `connect()` call failed at startup sat
idle for the entire run. With `-c400` and a tight listen backlog this caused
~153 dead connections, dropping measured RPS from ~10 000 to ~6 000 and
generating ~600 false timeouts (`check_timeouts` fired against connections whose
`c->start == 0` because they never sent a request).

## Changes

Applied to both `src/` and `baseline/src/` so `wrkx0` is fixed as well.

### `connect_socket()`: harden fd setup

- check `socket()` return; `goto error` on -1 (was: proceeded with `fd=-1`)
- check `fcntl(F_GETFL)` return; `goto error` on -1
  (was: passed garbage flags to `F_SETFL`, left socket blocking)
- check `fcntl(F_SETFL)` return; `goto error` on -1
- guard `close(fd)` with `fd >= 0`
- save `errno` around the error path so callers can inspect the cause
  after `close()` might overwrite it

### `delayed_initial_connect()`: retry selected TCP-level failures

- set `c->thread_start` once (first call only) so per-connection pacing
  and latency baseline reflect first scheduled time, not eventual
  successful connect time; missed slots surface as real latency pressure
- on failure: undo `connect_socket()`'s `errors.connect` increment if this
  connection was already counted, then branch on `errno`
- retryable (reschedule after `INITIAL_CONNECT_RETRY_MS` = 100 ms):
  - `ECONNREFUSED` — server sent RST; listen backlog full or port closed
  - `ETIMEDOUT`    — SYN dropped; backlog full and no RST sent (Linux default)
  - `ENETUNREACH`  — routing failure; may recover
  - `EHOSTUNREACH` — host unreachable; may recover
  - `ECONNRESET`   — connection reset after partial handshake
- non-retryable (`AE_NOMORE`, connection abandoned):
  - `EMFILE`/`ENFILE` — fd table exhausted; retrying at 100 ms/conn is futile
  - fcntl errors      — kernel internal failure; not a transient condition
  - ae errors         — event loop at capacity; not recoverable by retrying
- `errors.connect` at run end = number of unique connections that failed
  initial connect (regardless of cause or retry depth), not attempt count

## Validation

Server with explicit listen backlog of 16:

```nginx
# nginx.conf
events { worker_connections 1024; }
http { server { listen 8080 backlog=16; location / { return 200 "ok"; } } }
```

Or Python (no dependencies):

```python
# python3 minimal_server.py
import socket, threading
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', 8080)); s.listen(16)
def h(c):
    c.recv(4096)
    c.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'); c.close()
while True: threading.Thread(target=h, args=(s.accept()[0],), daemon=True).start()
```

System backlog ceiling (must be >= 16 or the kernel silently caps it):

```
Linux:  sysctl -w net.core.somaxconn=128 net.ipv4.tcp_max_syn_backlog=128
macOS:  sudo sysctl -w kern.ipc.somaxconn=128
```

Before patch (`baseline/wrkx0`):

```
./wrkx -t4 -c400 -d10s -R10000 http://127.0.0.1:8080
~5900-6200 RPS, errors.connect ~150, timeout ~600
```

After patch:

```
./wrkx -t4 -c400 -d10s -R10000 http://127.0.0.1:8080
~9800-10000 RPS, errors.connect = N unique connections that hit backlog=16
```

Reduce `backlog=` further or increase `-c` to force more initial failures and
verify the counter stays accurate.
