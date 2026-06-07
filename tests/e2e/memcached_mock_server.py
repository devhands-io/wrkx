#!/usr/bin/env python3
"""
Minimal memcached text-protocol mock server for wrkx E2E tests (ADR 0005, P4-1).

Usage: python3 memcached_mock_server.py <port>

Accepts TCP connections and speaks the memcached text protocol.
Handles pipelining (multiple commands arriving in one recv()).
Supports get, set, delete, incr, decr.

Storage model:
  SET stores the raw data bytes, keyed by name.
  INCR / DECR operate on keys whose stored value is a decimal integer; they
  return NOT_FOUND when the key is absent and CLIENT_ERROR when the value is
  not numeric.  This lets counter E2E tests verify both the happy path and the
  missing-key error path.
  DELETE removes the key; returns DELETED unconditionally (avoids NOT_FOUND
  churn in set/delete workloads that may race across connections).
  GET always returns a canned VALUE hit so the full VALUE…END parse path is
  exercised regardless of stored state.
"""

import sys
import socket
import threading
import argparse

# Shared key→value store (bytes).  Protected by storage_lock.
_storage      = {}
_storage_lock = threading.Lock()


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
        key   = parts[1] if len(parts) > 1 else ""
        delta = int(parts[2]) if len(parts) > 2 else 1
        return {"name": "incr", "key": key, "delta": delta}, crlf + 2

    if name == "decr":
        key   = parts[1] if len(parts) > 1 else ""
        delta = int(parts[2]) if len(parts) > 2 else 1
        return {"name": "decr", "key": key, "delta": delta}, crlf + 2

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
    """Return the reply bytes for a parsed command dict."""
    name = cmd["name"]

    if name == "get":
        # Always return a cache hit — GET tests don't care about stored state.
        key = cmd.get("key", "k").encode("ascii")
        return b"VALUE " + key + b" 0 5\r\nvalue\r\nEND\r\n"

    if name == "set":
        key  = cmd.get("key", "")
        data = cmd.get("data", b"")
        with _storage_lock:
            _storage[key] = data
        return b"STORED\r\n"

    if name == "delete":
        key = cmd.get("key", "")
        with _storage_lock:
            _storage.pop(key, None)
        return b"DELETED\r\n"

    if name == "incr":
        key   = cmd.get("key", "")
        delta = cmd.get("delta", 1)
        with _storage_lock:
            if key not in _storage:
                return b"NOT_FOUND\r\n"
            try:
                val = int(_storage[key]) + delta
            except (ValueError, TypeError):
                return b"CLIENT_ERROR cannot increment non-numeric value\r\n"
            _storage[key] = str(val).encode("ascii")
        return str(val).encode("ascii") + b"\r\n"

    if name == "decr":
        key   = cmd.get("key", "")
        delta = cmd.get("delta", 1)
        with _storage_lock:
            if key not in _storage:
                return b"NOT_FOUND\r\n"
            try:
                val = max(0, int(_storage[key]) - delta)
            except (ValueError, TypeError):
                return b"CLIENT_ERROR cannot decrement non-numeric value\r\n"
            _storage[key] = str(val).encode("ascii")
        return str(val).encode("ascii") + b"\r\n"

    return b"ERROR\r\n"


# ---------------------------------------------------------------------------
# Connection handler
# ---------------------------------------------------------------------------

def handle_conn(conn, addr, close_after=0, bad_reply=False):
    buf = b""
    cmd_count = 0
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
                cmd_count += 1
                if bad_reply:
                    conn.sendall(b"JUNK_NOT_VALID_REPLY\r\n")
                else:
                    conn.sendall(reply_for(cmd))
                if close_after > 0 and cmd_count >= close_after:
                    return  # force close after N commands
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
    parser = argparse.ArgumentParser(description="memcached mock server for wrkx E2E tests")
    parser.add_argument("port", type=int, help="TCP port to listen on")
    parser.add_argument("--close-after", type=int, default=0, metavar="N",
                        help="close each connection after N commands (0 = never)")
    parser.add_argument("--bad-reply", action="store_true",
                        help="send malformed replies to all commands")
    args = parser.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(128)

    while True:
        try:
            conn, addr = srv.accept()
        except OSError:
            break
        t = threading.Thread(
            target=handle_conn,
            args=(conn, addr, args.close_after, args.bad_reply),
            daemon=True,
        )
        t.start()


if __name__ == "__main__":
    main()
