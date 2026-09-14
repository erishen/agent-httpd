#!/usr/bin/env python3
"""Bridge this process's stdin/stdout to a TCP endpoint (MCP stdio over TCP).

Used as an agent-httpd MCP "command" inside the container: agent-httpd spawns
this as a normal stdio server, and it forwards bytes to a host-side relay
(deploy/mcp-host-relay.mjs) that owns the real MCP bridge. This lets a
host-native MCP server (needing uv/make/project dirs the container lacks) show
up as a local stdio tool.

Usage: mcp-tcp-client.py <host> <port>
"""

import os
import select
import socket
import sys

CHUNK = 65536


def write_all(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        view = view[n:]


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: mcp-tcp-client.py <host> <port>\n")
        return 2
    host = sys.argv[1]
    try:
        port = int(sys.argv[2])
    except ValueError:
        sys.stderr.write("mcp-tcp-client: bad port\n")
        return 2

    try:
        sock = socket.create_connection((host, port), timeout=30)
    except OSError as e:
        sys.stderr.write("mcp-tcp-client: connect %s:%d failed: %s\n" % (host, port, e))
        return 1
    sock.settimeout(None)

    try:
        while True:
            r, _, _ = select.select([0, sock], [], [])
            if 0 in r:
                data = os.read(0, CHUNK)
                if not data:
                    break  # stdin EOF -> child gone, shut down
                sock.sendall(data)
            if sock in r:
                data = sock.recv(CHUNK)
                if not data:
                    break  # relay closed
                write_all(1, data)
    except (OSError, ValueError):
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
