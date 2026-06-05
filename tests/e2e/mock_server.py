#!/usr/bin/env python3
"""
Minimal HTTP/1.1 mock server for wrkx E2E tests.

Usage: python3 mock_server.py <port> <mode>

Modes:
  instant   - 200 OK with no delay (smoke)
  delay     - 200 OK after a fixed 50 ms delay
  flaky     - 10% of responses return 503, rest 200 OK
  drop      - close the connection abruptly (tests reconnect patch)
  close     - 200 OK with Connection: close, then close (reconnect per request)
"""

import sys
import socket
import time
import random
import threading

RESPONSE_200 = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Length: 2\r\n"
    b"Connection: keep-alive\r\n"
    b"\r\n"
    b"OK"
)

RESPONSE_503 = (
    b"HTTP/1.1 503 Service Unavailable\r\n"
    b"Content-Length: 0\r\n"
    b"Connection: keep-alive\r\n"
    b"\r\n"
)

RESPONSE_200_CLOSE = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Length: 2\r\n"
    b"Connection: close\r\n"
    b"\r\n"
    b"OK"
)


def handle(conn, mode):
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break

            if mode == "instant":
                conn.sendall(RESPONSE_200)

            elif mode == "delay":
                time.sleep(0.05)
                conn.sendall(RESPONSE_200)

            elif mode == "flaky":
                if random.random() < 0.10:
                    conn.sendall(RESPONSE_503)
                else:
                    conn.sendall(RESPONSE_200)

            elif mode == "drop":
                conn.close()
                return

            elif mode == "close":
                conn.sendall(RESPONSE_200_CLOSE)
                conn.close()
                return

            else:
                conn.sendall(RESPONSE_200)
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <port> <mode>", file=sys.stderr)
        sys.exit(1)

    port = int(sys.argv[1])
    mode = sys.argv[2]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(64)

    while True:
        conn, _ = srv.accept()
        t = threading.Thread(target=handle, args=(conn, mode), daemon=True)
        t.start()


if __name__ == "__main__":
    main()
