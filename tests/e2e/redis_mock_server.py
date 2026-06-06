#!/usr/bin/env python3
"""
Minimal RESP mock server for wrkx Redis E2E tests (ADR 0005, P2-3/P2-4).

Usage: python3 redis_mock_server.py <port> [delay_ms]

  port      — TCP port to listen on
  delay_ms  — optional per-reply delay in milliseconds (default 0, instant)

Accepts TCP connections, speaks just enough RESP to satisfy the wrkx Redis
protocol vtable:
  - Handles AUTH and SELECT (issued in redis_connect() during connect handshake)
  - Handles PING, GET, SET, INCR, DEL and any unknown command
  - Handles pipelining (multiple commands arriving in one recv())

Replies are canned — the server does not store data.  This is sufficient to
exercise the full wrkx connection→write→readable→stats path without a real
Redis installation.
"""

import sys
import socket
import time
import threading

# Canned RESP replies, keyed by command name (upper-case bytes).
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
    """
    Try to consume one complete RESP bulk-array command from buf.

    Returns (command_name: bytes, consumed: int) on success, or
    (None, 0) when buf holds an incomplete command.

    Only handles the *N\\r\\n$len\\r\\narg\\r\\n... format that the wrkx
    Redis vtable sends — not arbitrary RESP values.
    """
    if not buf or buf[0:1] != b"*":
        # Not a bulk array; discard one byte and keep going so a stray
        # byte doesn't stall the connection forever.
        return None, 1

    crlf = buf.find(b"\r\n")
    if crlf < 0:
        return None, 0  # need more data

    try:
        count = int(buf[1:crlf])
    except ValueError:
        return None, 1  # malformed; skip

    pos = crlf + 2
    cmd = None
    for i in range(count):
        if pos >= len(buf) or buf[pos:pos + 1] != b"$":
            return None, 0  # incomplete
        crlf2 = buf.find(b"\r\n", pos + 1)
        if crlf2 < 0:
            return None, 0
        try:
            arglen = int(buf[pos + 1:crlf2])
        except ValueError:
            return None, 1
        pos = crlf2 + 2
        if pos + arglen + 2 > len(buf):
            return None, 0  # data not yet arrived
        if i == 0:
            cmd = buf[pos:pos + arglen].upper()
        pos += arglen + 2   # skip content + \r\n

    return cmd, pos


def handle(conn, delay_s=0.0):
    buf = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            # Drain all complete commands from the buffer (handles pipelining).
            while buf:
                cmd, consumed = parse_resp_command(buf)
                if consumed == 0:
                    break   # need more data from the network
                buf = buf[consumed:]
                if cmd is not None:
                    if delay_s > 0:
                        time.sleep(delay_s)
                    conn.sendall(REPLIES.get(cmd, DEFAULT_REPLY))
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    if len(sys.argv) not in (2, 3):
        print(f"Usage: {sys.argv[0]} <port> [delay_ms]", file=sys.stderr)
        sys.exit(1)

    port     = int(sys.argv[1])
    delay_s  = int(sys.argv[2]) / 1000.0 if len(sys.argv) == 3 else 0.0

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(128)

    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        t = threading.Thread(target=handle, args=(conn, delay_s), daemon=True)
        t.start()


if __name__ == "__main__":
    main()
