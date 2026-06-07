#!/usr/bin/env python3
"""
Minimal memcached text-protocol mock server for wrkx E2E tests (ADR 0005, P4-1).

Usage: python3 memcached_mock_server.py <port>

Accepts TCP connections and speaks the memcached text protocol.
Canned replies — no real storage.  Handles pipelining (multiple commands
arriving in one recv()).  Supports get, set, delete, incr, decr.

GET always returns a cache hit so the full VALUE…END parse path is exercised.
"""

import sys
import socket
import threading


# ---------------------------------------------------------------------------
# Text-protocol parser
# ---------------------------------------------------------------------------

def parse_command(buf):
    """
    Try to consume one complete memcached text command from buf (bytes).

    Returns (cmd_dict, consumed_bytes) on success, or (None, 0) when buf
    holds an incomplete command.

    cmd_dict keys: 'name' (str, lower-case), and op-specific extras.
    """
    crlf = buf.find(b"\r\n")
    if crlf < 0:
        return None, 0

    try:
        line = buf[:crlf].decode("ascii")
    except UnicodeDecodeError:
        return {"name": "error"}, crlf + 2

    parts = line.split()
    if not parts:
        return {"name": "error"}, crlf + 2

    name = parts[0].lower()

    if name in ("get", "gets"):
        key = parts[1] if len(parts) > 1 else ""
        return {"name": "get", "key": key}, crlf + 2

    if name == "delete":
        key = parts[1] if len(parts) > 1 else ""
        return {"name": "delete", "key": key}, crlf + 2

    if name == "incr":
        key = parts[1] if len(parts) > 1 else ""
        return {"name": "incr", "key": key}, crlf + 2

    if name == "decr":
        key = parts[1] if len(parts) > 1 else ""
        return {"name": "decr", "key": key}, crlf + 2

    if name == "set":
        # set <key> <flags> <exptime> <bytes> [noreply]\r\n<data>\r\n
        try:
            nbytes = int(parts[4])
        except (IndexError, ValueError):
            return {"name": "error"}, crlf + 2
        total = crlf + 2 + nbytes + 2
        if len(buf) < total:
            return None, 0   # data block not yet arrived
        data = buf[crlf + 2: crlf + 2 + nbytes]
        return {"name": "set", "key": parts[1], "data": data}, total

    return {"name": "unknown"}, crlf + 2


def reply_for(cmd):
    """Return the canned bytes reply for a parsed command dict."""
    name = cmd["name"]
    if name == "get":
        key = cmd.get("key", "k").encode()
        return b"VALUE " + key + b" 0 5\r\nvalue\r\nEND\r\n"
    if name == "set":
        return b"STORED\r\n"
    if name == "delete":
        return b"DELETED\r\n"
    if name == "incr":
        return b"1\r\n"
    if name == "decr":
        return b"0\r\n"
    return b"ERROR\r\n"


# ---------------------------------------------------------------------------
# Connection handler
# ---------------------------------------------------------------------------

def handle_conn(conn, addr):
    buf = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            # Process all complete commands in the buffer.
            while buf:
                cmd, consumed = parse_command(buf)
                if cmd is None:
                    break   # incomplete — wait for more data
                buf = buf[consumed:]
                conn.sendall(reply_for(cmd))
    except (OSError, BrokenPipeError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <port>", file=sys.stderr)
        sys.exit(1)

    port = int(sys.argv[1])

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(128)

    while True:
        try:
            conn, addr = srv.accept()
        except OSError:
            break
        t = threading.Thread(target=handle_conn, args=(conn, addr), daemon=True)
        t.start()


if __name__ == "__main__":
    main()
