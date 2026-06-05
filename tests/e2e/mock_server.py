#!/usr/bin/env python3
"""
Minimal HTTP/1.1 mock server for wrkx E2E tests.

Usage: python3 mock_server.py <port> <mode> [N]

Modes:
  instant   - 200 OK with no delay (smoke)
  delay     - 200 OK after a fixed 50 ms delay
  flaky     - 10% of responses return 503, rest 200 OK
  drop      - close the connection abruptly (tests reconnect patch)
  close     - 200 OK with Connection: close, then close (reconnect per request)
  kalimit   - keep-alive, but close each connection after N responses (default
              1000) by sending Connection: close on the Nth, like nginx's
              keepalive_requests. Exercises graceful-close handling (ADR 0003-B).
              N is the optional 3rd argument.
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


def handle(conn, mode, kalimit_n=1000):
    served = 0
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break

            if mode == "kalimit":
                served += 1
                if served >= kalimit_n:
                    # Final allowed keepalive request: close gracefully.
                    conn.sendall(RESPONSE_200_CLOSE)
                    conn.close()
                    return
                conn.sendall(RESPONSE_200)

            elif mode == "instant":
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
    if len(sys.argv) not in (3, 4):
        print(f"Usage: {sys.argv[0]} <port> <mode> [N]", file=sys.stderr)
        sys.exit(1)

    port = int(sys.argv[1])
    mode = sys.argv[2]
    kalimit_n = int(sys.argv[3]) if len(sys.argv) == 4 else 1000

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(64)

    while True:
        conn, _ = srv.accept()
        t = threading.Thread(target=handle, args=(conn, mode, kalimit_n),
                             daemon=True)
        t.start()


if __name__ == "__main__":
    main()
