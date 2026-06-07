#!/usr/bin/env python3
"""
Recording Redis mock server for the Gate D parity test (ADR 0005, t076).

Usage: python3 redis_recording_mock.py <port> <log_file>

Behaves like redis_mock_server.py but appends each received command as a
tab-separated line to <log_file>:

    VERB<TAB>ARG1<TAB>ARG2...

Lines are flushed immediately so the parent process can read them after
the test run completes.
"""

import sys
import socket
import threading


REPLIES = {
    b"PING":   b"+PONG\r\n",
    b"SET":    b"+OK\r\n",
    b"GET":    b"$5\r\nvalue\r\n",
    b"AUTH":   b"+OK\r\n",
    b"SELECT": b"+OK\r\n",
    b"INCR":   b":1\r\n",
    b"DEL":    b":1\r\n",
}
DEFAULT_REPLY = b"+OK\r\n"


def parse_resp_command(buf):
    """Consume one complete RESP bulk-array from buf.
    Returns (args: list[bytes], consumed: int) or (None, 0) on incomplete."""
    if not buf or buf[0:1] != b"*":
        return None, 1

    crlf = buf.find(b"\r\n")
    if crlf < 0:
        return None, 0

    try:
        count = int(buf[1:crlf])
    except ValueError:
        return None, 1

    pos = crlf + 2
    args = []
    for _ in range(count):
        if pos >= len(buf) or buf[pos:pos + 1] != b"$":
            return None, 0
        crlf2 = buf.find(b"\r\n", pos + 1)
        if crlf2 < 0:
            return None, 0
        try:
            arglen = int(buf[pos + 1:crlf2])
        except ValueError:
            return None, 1
        pos = crlf2 + 2
        if pos + arglen + 2 > len(buf):
            return None, 0
        args.append(buf[pos:pos + arglen])
        pos += arglen + 2

    return args, pos


def handle(conn, log_lock, log_file):
    buf = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            while buf:
                args, consumed = parse_resp_command(buf)
                if consumed == 0:
                    break
                buf = buf[consumed:]
                if args:
                    cmd = args[0].upper()
                    conn.sendall(REPLIES.get(cmd, DEFAULT_REPLY))
                    # Log: skip AUTH and SELECT (they are handshake commands,
                    # not workload commands, and differ between runs).
                    if cmd not in (b"AUTH", b"SELECT"):
                        line = "\t".join(a.decode("utf-8", errors="replace")
                                        for a in args)
                        with log_lock:
                            log_file.write(line + "\n")
                            log_file.flush()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <port> <log_file>", file=sys.stderr)
        sys.exit(1)

    port     = int(sys.argv[1])
    log_path = sys.argv[2]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(128)

    log_lock = threading.Lock()
    with open(log_path, "w") as lf:
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                break
            t = threading.Thread(target=handle,
                                 args=(conn, log_lock, lf),
                                 daemon=True)
            t.start()


if __name__ == "__main__":
    main()
